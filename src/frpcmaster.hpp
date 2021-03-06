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
#include "manager.hpp"
#include "easylogging++.h"

#include <iostream>
#include <queue>
#include <string>
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
    int buffer_idx;

    // 和frps的连接socket 以及管理和服务的连接
    short local_port, remote_port, frps_port;
    string frps_ip;
    int connection;
    struct sockaddr_in frps_addr, local;

    // 侦听
    int epollfd;

    // 和frps建立连接
    void Connect();
    int send_port(short remote_port);
    int send_heart_beat();
    int conn_need();
    // 分配连接来进行管理
    void arrange_new_pair(int &local_conn, int &remote_conn);

    // 和frps的通信
    RET_CODE read_from_frps();
    int read_from_buffer();
    RET_CODE write_to_frps();
    RET_CODE write_to_buffer(short remote_port);

    // 心跳包计数
    int heart_count;

public:
    Master(const string& serv_ip, short serv_port, short local_port, short remote_port);
    ~Master();
    void start();    
};

const int Master::BUFFER_SIZE = 512;
const int Master::EVENTS_SIZE = 5;
const int Master::TIME_OUT = 30000;
const int Master::MAX_HEART_BEATS = 3;

Master::Master(const string& serv_ip, short serv_port, short port1, short port2):
        frps_ip(serv_ip), frps_port(serv_port), local_port(port1), 
        remote_port(port2), buffer_idx(0), heart_count(0){
    buffer = new char[BUFFER_SIZE];
    epollfd = epoll_create(1);

    bzero(&frps_addr, sizeof(frps_addr));
    frps_addr.sin_family = AF_INET;
    inet_pton(AF_INET, frps_ip.c_str(), &frps_addr.sin_addr);
    frps_addr.sin_port = htons(frps_port);

    bzero(&local, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    // inet_pton(AF_INET, "192.168.66.18", &local.sin_addr);
    local.sin_port = htons(local_port);
}

Master::~Master(){
    delete []buffer;
    close(epollfd);
    close(connection);
}

void Master::start(){
    Connect();
    bool stop = false;
    epoll_event events[EVENTS_SIZE];
    int num, res;
    int local_conn, remote_conn;
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
                res = send_port(remote_port);
                if(res==-1){
                    stop = true;
                    break;
                }
                modfd(epollfd, connection, EPOLLIN);
            }
            else if(events[i].data.fd==connection && (events[i].events & EPOLLIN)){
                res = conn_need();
                if(res<=0){
                    stop = true;
                    break;
                }
                LOG(INFO) << "the conn needed by frps" << res;
                // 创建线程来管理转发任务
                for(int i=0;i<res;++i){
                    arrange_new_pair(local_conn, remote_conn);
                    Manager* manager = new Manager(remote_conn, local_conn);
                    int ret = ret=pthread_create(&tid, nullptr, Manager::start_routine, (void*)manager);
                    if(ret!=0){
                    perror("pthread_create failed!");
                    exit(1);
                    }
                    ret=pthread_detach(tid);
                    if(ret!=0){
                        perror("pthread_detach failed!");
                        exit(1);
                    }
                }
                // 不行这个后续好像没法继续监听读事件
                modfd(epollfd, connection, EPOLLIN);
            }else{
                LOG(ERROR) << "the wrong type coming to frpc";
            }
        }
    }
}

void Master::Connect(){
    connection = socket(PF_INET, SOCK_STREAM, 0);
    if(connection<0){
        perror("create socket failed!");
        exit(1);
    }
    int ret=connect(connection, (struct sockaddr*)&frps_addr, sizeof(frps_addr));
    if(ret!=0){
        perror("connect failed!");
        close(connection);
        exit(1);
    }
    // 准备发送配置信息
    add_writefd(epollfd, connection);
}

int Master::send_port(short remote_port){
    RET_CODE res = write_to_buffer(remote_port);
    if(res==BUFFER_FULL){
        LOG(ERROR) << "the buffer is not enough";
        exit(1);
    }
    res = write_to_frps();
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
    res = write_to_frps();
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

int Master::conn_need(){
    RET_CODE res = read_from_frps();
    switch(res){
        case BUFFER_FULL:{
            LOG(ERROR) << "the buffer is not enough to read";
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
    int num = read_from_buffer();
    if(num==0){
        LOG(INFO) << "the conn_need is 0";
    }
    return num;
}

// 进行连接分配管理
void Master::arrange_new_pair(int &local_conn, int &frps_conn){
    // 设置线程
    // 连接本地22端口
    local_conn = socket(PF_INET, SOCK_STREAM, 0);
    if(local_conn<0){
        perror("create socket failed!");
        exit(1);
    }
    int ret=connect(local_conn, (struct sockaddr*)&local, sizeof(local));
    if(ret!=0){
        perror("connect failed!");
        close(local_conn);
        exit(1);
    }

    // 和frps连接
    frps_conn = socket(PF_INET, SOCK_STREAM, 0);
    if(frps_conn<0){
        perror("create socket failed!");
        exit(1);
    }
    ret=connect(frps_conn, (struct sockaddr*)&frps_addr, sizeof(frps_addr));
    if(ret!=0){
        perror("connect failed!");
        close(frps_conn);
        exit(1);
    }
}

// 读取从frps发来的连接需求数量
RET_CODE Master::read_from_frps(){
    buffer_idx = 0;
    memset(buffer, '\0', BUFFER_SIZE);
    int bytes_read;
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
        buffer_idx+=bytes_read;
    }
    return (buffer_idx>0)?buffer_idx=0, OK: NOTHING;
}

int Master::read_from_buffer(){
    int num = *((int*)buffer);
    memset(buffer, '\0', BUFFER_SIZE);
    return num;
}

RET_CODE Master::write_to_frps(){
    int bytes_write = 0, length = sizeof(remote_port);
    buffer_idx = 0;
    while(true){
        if(buffer_idx>=length){
            buffer_idx = 0;
            memset(buffer, '\0', BUFFER_SIZE);
            return BUFFER_EMPTY;
        }
        // 防止连接关闭去写导致程序崩溃
        bytes_write = send(connection, buffer+buffer_idx, length-buffer_idx, MSG_NOSIGNAL);
        if(bytes_write==-1){
            if(errno==EAGAIN || errno==EWOULDBLOCK){
                return TRY_AGAIN;
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
