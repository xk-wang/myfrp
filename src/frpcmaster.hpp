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

// 功能如下：
// 建立和frps的连接
// 发送配置端口信息，建立remote_port-->local_port映射
// 侦听frps发来的连接数量
// 分配manager来管理frps的连接与本地22端口连接

// master只负责去侦听和frps的connection
class Master{
private:
    static const int BUFFER_SIZE;
    static const int EVENTS_SIZE;
    static const int TIME_OUT;
    static const int MAX_HEART_BEATS;
    char* buffer;

    // 和frps的连接socket 以及管理和服务的连接
    struct sockaddr_in frps_addr;
    // remote_port-->sockaddr_in的映射
    unordered_map<short, struct sockaddr_in>locals;
    int connection;

    // 侦听
    int epollfd;

    // 和frps建立连接
    void Connect();
    int send_port();
    int send_heart_beat();
    // 分配连接来进行管理
    void arrange_new_pair(short remote_port, int &remote_conn);

    // 和frps的通信
    RET_CODE write_to_frps(int length);
    RET_CODE write_to_buffer(short message);
    int read_conn_from_buffer(short& remote_port, short& conns);
    RET_CODE write_ports_to_buffer();

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

    bzero(&frps_addr, sizeof(frps_addr));
    frps_addr.sin_family = AF_INET;
    inet_pton(AF_INET, serv_ip.c_str(), &frps_addr.sin_addr);
    frps_addr.sin_port = htons(serv_port);

    // 构建remote_port-->struct sockaddr_in的映射
    short remote_port, local_port;
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
    Connect();
    bool stop = false;
    epoll_event events[EVENTS_SIZE];
    int num, res;
    pthread_t tid;
    while(!stop){
        num = epoll_wait(epollfd, events, EVENTS_SIZE, TIME_OUT);
        if(num==-1){
            perror("epoll_wait failed!");
            exit(1);
        }
        // 30s没connection的读写事件发生，需要发送心跳包
        if(num==0){
            res = send_heart_beat();
            if(res==-1) ++heart_count;
            else heart_count=0;
            if(heart_count>=MAX_HEART_BEATS){
                stop = true;
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
                modfd(epollfd, connection, EPOLLIN);
            }
            // 读取内容格式为remote_port, conn_need每次发送一个服务的
            else if(events[i].data.fd==connection && (events[i].events & EPOLLIN)){
                short remote_port, conns;
                res = read_conn_from_buffer(remote_port, conns);
                if(res==-1){
                    break;
                }
                LOG(INFO) << "remote_port " << remote_port << " need " << conns << "connections";
                // 创建线程来管理转发任务
                int remote_conn;
                for(int i=0;i<conns;++i){
                    arrange_new_pair(remote_port, remote_conn);
                    FRPCManager* manager = new FRPCManager(remote_conn, locals);
                    int ret = ret=pthread_create(&tid, nullptr, FRPCManager::start_frpc_routine, (void*)manager);
                    if(ret!=0){
                        close_file(remote_conn);
                        LOG(ERROR) << "-->" <<remote_port << " pthread_create failed!";
                    }
                    ret=pthread_detach(tid);
                    if(ret!=0){
                        close_file(remote_conn);
                        LOG(ERROR) << "-->" <<remote_port << " pthread_detach failed!";
                    }
                }
                // modfd(epollfd, connection, EPOLLIN);
            }else{
                LOG(ERROR) << "the wrong type coming to frpc";
            }
        }
    }
}

void Master::Connect(){
    connection = socket(PF_INET, SOCK_STREAM, 0);
    if(connection<0){
        LOG(ERROR) << "create socket failed!";
        exit(1);
    }
    int ret=connect(connection, (struct sockaddr*)&frps_addr, sizeof(frps_addr));
    if(ret!=0){
        LOG(ERROR) << "connect frps failed!";
        close_file(connection);
        exit(1);
    }
    // 准备发送配置信息
    add_writefd(epollfd, connection);
}


// 通信格式为port_len, port1, port2, ...均使用short型来发送
int Master::send_port(){
    RET_CODE res = write_ports_to_buffer();
    if(res==BUFFER_FULL){
        LOG(ERROR) << "the buffer is not enough";
        exit(1);
    }
    res = write_to_frps(sizeof(short)*(locals.size()+1));
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
    RET_CODE res = write_to_buffer(-2);
    if(res==BUFFER_FULL){
        LOG(ERROR) << "the buffer is not enough";
        exit(1);
    }
    res = write_to_frps(sizeof(short));
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
void Master::arrange_new_pair(short remote_port, int &serv_conn){
    // 设置线程
    // 和frps相关的服务来连接比如32318端口
    serv_conn = socket(PF_INET, SOCK_STREAM, 0);
    if(serv_conn<0){
        LOG(ERROR) << "create serv_conn socket failed!";
        return;
    }
    frps_addr.sin_port = htons(remote_port);
    ret=connect(serv_conn, (struct sockaddr*)&frps_addr, sizeof(frps_addr));
    if(ret!=0){
        LOG(ERROR) << "connect remote_port " << remote_port << " failed!";
        close_file(serv_conn);
        return;
    }
}


int Master::read_conn_from_buffer(short& remote_port, short& conns){
    short* p = (short*)buffer;
    remote_port = *p++;
    if(remote_port<=0){
        LOG(ERROR) << "the remote port received <=0";
        return -1;
    }
    conns = *p;
    if(conns<=0){
        LOG(ERROR) << "the conns need by remote_port " << remote_port << " is <=0";
        return -1;
    }
    memset(buffer, '\0', BUFFER_SIZE);
    return 0;
}

RET_CODE Master::write_to_frps(int length){
    int bytes_write = 0;
    int buffer_idx = 0;
    while(true){
        if(buffer_idx>=length){
            buffer_idx = 0;
            memset(buffer, '\0', BUFFER_SIZE);
            return BUFFER_EMPTY;
        }
        // 防止连接关闭去写导致程序崩溃
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

RET_CODE Master::write_to_buffer(short message){
    if(BUFFER_SIZE<sizeof(message)) return BUFFER_FULL;
    memset(buffer, '\0', BUFFER_SIZE);
    *((short*)buffer) = message;
    return OK;
}

RET_CODE Master::write_ports_to_buffer(){
    if(locals.size()>65535){
        LOG(ERROR) << "the max service port num is 65536";
        exit(1);
    }
    short port_num = locals.size();
    if((port_num+1)*sizeof(short)>BUFFER_SIZE) return BUFFER_FULL;
    memset(buffer, '\0', BUFFER_SIZE);
    short* p = (short*)buffer;
    *p++ = port_num;
    for(auto iter: locals) *p++ = iter.first;
    return OK;
}
