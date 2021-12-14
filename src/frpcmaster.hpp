#pragma once

#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "util.hpp"
#include "frpcmanager.hpp"
#include "easylogging++.h"

#include <iostream>
#include <queue>
#include <string>
#include <unordered_map>
using namespace std;

// 功能：
// 1.主动建立和frps的固定连接connection
// 2.发送remote_ports，建立remote_port-->local_addr映射
// 3.侦听connection发来的连接需求
// 4.分配manager将本地连接和远程连接绑定
// 5.空闲时发送心跳

class Master{
private:
    //常量设置
    static const int BUFFER_SIZE;
    static const int EVENTS_SIZE;
    static const int TIME_OUT;
    static const int MAX_HEART_BEATS;
    // 缓冲区资源
    char* buffer;

    // 向服务端发起来接的地址
    struct sockaddr_in frps_addr;
    // remote_port-->sockaddr_in的映射
    unordered_map<short, struct sockaddr_in>locals;
    // 和frpc的固定连接
    int connection;

    // 侦听
    int epollfd;
    // 和frps建立连接
    int Connect();
    // 发送端口配置信息
    int send_port();
    // 发送心跳
    int send_heart_beat();
    // 读取连接需求数量
    int read_conn(short& conns);
    // 分配新的远端连接
    int arrange_new_pair(int &remote_conn);

    // 向frps写
    RET_CODE write_to_frps(int length);
    // 从frps读
    RET_CODE read_from_frps();
    // 心跳包计数
    int heart_count;

public:
    Master(const string& serv_ip, short serv_port, const unordered_map<short, short>&ports);
    ~Master();
    void start();    
};

const int Master::BUFFER_SIZE = 512;
const int Master::EVENTS_SIZE = 5;
const int Master::TIME_OUT = 30000;
const int Master::MAX_HEART_BEATS = 3;

Master::Master(const string& serv_ip, short serv_port, const unordered_map<short, short>&ports):
        heart_count(0){
    buffer = new char[BUFFER_SIZE];
    epollfd = epoll_create(1);

    // 记录frps_addr
    bzero(&frps_addr, sizeof(frps_addr));
    frps_addr.sin_family = AF_INET;
    inet_pton(AF_INET, serv_ip.c_str(), &frps_addr.sin_addr);
    frps_addr.sin_port = htons(serv_port);

    // 构建remote_port-->struct sockaddr_in的映射
    struct sockaddr_in local_addr;
    for(auto iter: ports){
        short remote_port = iter.first, local_port = iter.second;
        bzero(&local_addr, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        local_addr.sin_port = htons(local_port);
        locals[remote_port] = local_addr;
    }
}

Master::~Master(){
    delete []buffer;
    close_file(epollfd);
    close_file(connection);
}

void Master::start(){
    connection=Connect();
    if(connection==-1) return;
    bool stop = false;
    epoll_event events[EVENTS_SIZE];
    int num, res;
    pthread_t tid;
    while(!stop){
        num = epoll_wait(epollfd, events, EVENTS_SIZE, TIME_OUT);
        if(num==-1){
            perror("epoll_wait failed!");
            break;
        }
        // 30s没connection的读写事件发生，需要发送心跳包
        if(num==0){
            res = send_heart_beat();
            if(res==-1) ++heart_count;
            else heart_count=0;
            if(heart_count>=MAX_HEART_BEATS){
                break;
            }
        }
        for(int i=0;i<num;++i){
            if(events[i].data.fd==connection && (events[i].events & EPOLLOUT)){
                res = send_port();
                if(res==-1){
                    stop = true;
                    break;
                }
                // 进行数据的侦听
                modfd(epollfd, connection, EPOLLIN);
            }
            // 读取内容格式为conn_need 至于是给具体哪个服务在子线程内部决定
            else if(events[i].data.fd==connection && (events[i].events & EPOLLIN)){
                short conns;
                res = read_conn(conns);
                if(res==-1){
                   continue; // 如果出现错误直接忽略本次的请求
                }
                LOG(INFO) << "remote_port need " << conns << " connections";
                // 创建线程来管理转发任务
                int remote_conn;
                for(int i=0;i<conns;++i){
                    res=arrange_new_pair(remote_conn);
                    if(res==-1) continue;
                    FRPCManager* manager = new FRPCManager(remote_conn, locals);
                    int ret = ret=pthread_create(&tid, nullptr, FRPCManager::start_frpc_routine, (void*)manager);
                    if(ret!=0){
                        close_file(remote_conn);
                        LOG(ERROR) << "pthread_create failed!";
                    }
                    ret=pthread_detach(tid);
                    if(ret!=0){
                        close_file(remote_conn);
                        LOG(ERROR) << "pthread_detach failed!";
                    }
                }
                // modfd(epollfd, connection, EPOLLIN);
            }else{
                LOG(ERROR) << "the wrong type coming to frpc";
            }
        }
    }
}

int Master::Connect(){
    int connection = socket(PF_INET, SOCK_STREAM, 0);
    if(connection<0){
        LOG(ERROR) << "create socket failed!";
        return -1;
    }
    int ret=connect(connection, (struct sockaddr*)&frps_addr, sizeof(frps_addr));
    if(ret!=0){
        LOG(ERROR) << "connect frps failed!";
        close_file(connection);
        return -1;
    }
    // 准备发送配置信息
    add_writefd(epollfd, connection);
    return connection;
}


// 通信格式为port_len, port1, port2, ...均使用short型来发送
int Master::send_port(){
    // 填充数据
    if(locals.size()>65535){
        LOG(ERROR) << "the max service port num is 65536";
        return -1;
    }
    short port_num = locals.size();
    if((port_num+1)*sizeof(short)>BUFFER_SIZE){
        LOG(ERROR) << "the buffer is not enough";
        return -1;
    }
    short* p = (short*)buffer;
    *p++ = port_num;
    for(auto iter: locals) *p++ = iter.first;

    // 通过网络进行数据发送
    RET_CODE res = write_to_frps(sizeof(short)*(port_num+1));
    switch(res){
        case IOERR:{
            LOG(ERROR) << "the frps error";
            return -1;
        }
        case CLOSED:{
            LOG(ERROR) << "the frps closed";
            return -1;
        }
        case TRY_AGAIN:{
            LOG(ERROR) << "the kernel is not enough to send remote_port";
            return -1;
        }
        default:
            break;
    }
    return 0;
}

int Master::send_heart_beat(){
    if(BUFFER_SIZE<sizeof(short)){
        LOG(ERROR) << "the buffer is not enough to send heart beat";
        return -1;
    };
    *((short*)buffer) = -2;

    RET_CODE res = write_to_frps(sizeof(short));
    switch(res){
        case IOERR:{
            LOG(ERROR) << "the frps error";
            return -1;
        }
        case CLOSED:{
            LOG(ERROR) << "the frps closed";
            return -1;
        }
        case TRY_AGAIN:{
            LOG(ERROR) << "the kernel is not enough to send heart beat pack";
            return -1;
        }
        default:
            break;
    }
    return 0;
}

// 进行连接分配管理
int Master::arrange_new_pair(int &serv_conn){
    serv_conn = socket(PF_INET, SOCK_STREAM, 0);
    if(serv_conn<0){
        LOG(ERROR) << "create serv_conn socket failed!";
        return -1;
    }
    // 再次发起连接请求
    int ret=connect(serv_conn, (struct sockaddr*)&frps_addr, sizeof(frps_addr));
    if(ret!=0){
        LOG(ERROR) << "connect remote failed!";
        close_file(serv_conn);
        return -1;
    }
    return 0;
}


int Master::read_conn(short& conns){
    // 先从frps中进行数据读取
    RET_CODE res = read_from_frps();
    switch(res){
        case BUFFER_FULL:{
            LOG(ERROR) << "the buffer is not enough to receive data";
            return -1;
        }
        case IOERR:{
            LOG(ERROR) << "the frps error";
            return -1;
        }
        case CLOSED:{
            LOG(ERROR) << "the frps closed";
            return -1;
        }
        default:
            break;
    }
    // 进行数据的解析
    short* p = (short*)buffer;
    conns = *p;
    if(conns<=0){
        LOG(ERROR) << "the conns need by remote_port is <=0";
        return -1;
    }
    return 0;
}

//
RET_CODE Master::write_to_frps(int length){
    int bytes_write = 0;
    int buffer_idx = 0;
    while(true){
        if(buffer_idx>=length){
            return BUFFER_EMPTY;
        }
        // send返回0确实是服务器关闭了连接
        int retry = 0;
        label:
        bytes_write = send(connection, buffer+buffer_idx, length-buffer_idx, MSG_NOSIGNAL);
        if(bytes_write==-1){
            // 等候缓冲区
            if(errno==EAGAIN || errno==EWOULDBLOCK){
                ++retry;
                if(retry>5){
                    return TRY_AGAIN;
                }
                goto label;
            }
            return IOERR;
        }
        else if(bytes_write==0) return CLOSED;
        buffer_idx += bytes_write;
    }
    return OK;
}

// 从frps读
RET_CODE Master::read_from_frps(){
    int bytes_read = 0;
    int buffer_idx = 0;
    while(true){
        if(buffer_idx>=BUFFER_SIZE){
            return BUFFER_FULL;
        }
        bytes_read = recv(connection, buffer+buffer_idx, BUFFER_SIZE-buffer_idx, 0);
        if(bytes_read==-1){
            if(errno==EAGAIN || errno==EWOULDBLOCK) break;
            return IOERR;
        }
        else if(bytes_read==0) return CLOSED;
        buffer_idx += bytes_read;
    }
    return buffer_idx>0? OK: NOTHING;
}