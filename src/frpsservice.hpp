#pragma once 

#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/sockets.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "util.hpp"
#include "manager.hpp"
#include "easylogging++.h"

#include <iostream>
#include <queue>
using namespace std;

// 负责侦听service_listenfd 接收链接并且将连接分配给子线程
class Service{
private:

    // 缓冲区来发送连接请求
    static const int BUFFER_SIZE;
    static const int EVENTS_SIZE;
    char* buffer;

    // 保存frpc的地址来进行区分客户端和client的连接
    struct sockaddr_in serv_addr;
    int listenfd;
    int epollfd;
    int connection; // 长连接，用来向frpc发送字节

    // 连接管理
    queue<int>clients;
    queue<int>frpcs;

    // 功能
    void Listen();
    void connection_request();
    bool is_frpc(const struct sockaddr_in& client);
    RET_CODE write_to_buffer(short num);
    RET_CODE write_to_frpc(int length);

public:
    Service(int listen_port, int conn);
    ~Service();
    void start();
};

const int Service::BUFFER_SIZE = 512;
const int Service::EVENTS_SIZE = 5;

// connection是每个服务独占的
Service::Service(int listen_port, int conn): connection(conn)){
    buffer = new char[BUFFER_SIZE];
    bzero(&serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(listen_port);

    epollfd = epoll_create(1);
}

Service::~Service(){
    delete []buffer;
    close_file(listenfd);
    close_file(epollfd);
}

void Service::start(){
    Listen();

    bool stop = false;
    epoll_event events[EVENTS_SIZE];
    int num;
    RET_CODE res;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    short conn_need = 0;
    while(!stop){
        // 侦听listenfd的读事件和connection的写事件， 暂时没有超时需要考虑
        num = epoll_wait(epollfd, events, EVENTS_SIZE, -1);
        if(num==-1){
            perror("epoll_wait failed");
            exit(1);
        }
        for(int i=0;i<num;++i){
            // 客户端连接
            if(events[i].data.fd==listenfd && (events[i].events & EPOLLIN)){
                int conn = accept(listenfd, (struct sockaddr*)&client_addr, &client_len);
                if(conn==-1){
                    perror("accept failed!");
                    exit(1);
                }
                if(is_frpc(client_addr)){
                    LOG(INFO) << "received frpc connection for service";
                    frpcs.push(conn);
                }
                else{
                    LOG(INFO) << "received client connection";
                    clients.push(conn);
                    ++conn_need;
                    modfd(epollfd, connection, EPOLLOUT);
                }
            }
            // 发请求连接的报文
            else if(events[i].data.fd==connection && (events[i].events & EPOLLOUT)){
                if(clients.empty()){
                    LOG(INFO) << "no connections from frpc need for clients";
                    continue;
                }
                // 需要发送的连接数需求不是client.size
                res = write_to_buffer(conn_need);
                if(res==BUFFER_FULL){
                    stop = true;
                    break;
                }
                res = write_to_frpc(2*sizeof(short));
                // 长连接关闭意味着无法与frpc进行通信，相应的服务没法进行
                // 需要关闭长连接和服务侦听
                switch(res){
                    case IOERR:{
                        close_fd(epollfd, connection);
                        close_fd(epollfd, listenfd);
                        connection = -1;
                        LOG(ERROR) << "the important connection error!";
                        break;
                    }
                    case CLOSED:{
                        close_fd(epollfd, connection);
                        close_fd(epollfd, listenfd);
                        connection = -1;
                        LOG(ERROR) << "the important connection closed!";
                        break;
                    }
                    case TRY_AGAIN:{
                        close_fd(epollfd, connection);
                        close_fd(epollfd, listenfd);
                        connection = -1;
                        LOG(ERROR) << "the kernel is not enough to write!";
                        break;
                    }
                    case BUFFER_EMPTY:{
                        conn_need = 0;
                        LOG(INFO) << "already write the need connection to frpc";
                    }
                    default:
                        break;
                }
            }
            else{
                LOG(ERROR) << "the wrong service event comming";
            }
        }
    }
}

void Service::Listen(){
    listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if(listenfd<0){
        perror("create service socket failed!");
        exit(1);
    }
    // int opt=1;
    // setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void*)&opt, sizeof(opt));

    int ret=bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    if(ret==-1){
        perror("bind failed!");
        exit(1);
    }
    ret = listen(listenfd, 5);
    if(ret==-1){
        perror("listen failed!");
        exit(1);
    }
    LOG(INFO) << "the service listening: " << port;
    add_readfd(epollfd, listenfd);
}

bool Service::is_frpc(const struct sockaddr_in& client){
    return frpc_addr.sin_addr.s_addr == client.sin_addr.s_addr &&
           frpc_addr.sin_port == client.sin_port;
}


// 发送 端口 需求连接数量
RET_CODE Service::write_to_buffer(short num){
    if(2*sizeof(short)>BUFFER_SIZE){
        LOG(ERROR) << "the buffer is not enough to send remote_port and conns";
        exit(1);
    }
    memeset(buffer, '\0', BUFFER_SIZE);
    short* p = (short*)buffer;
    *p++ = ntohs(serv_addr.sin_port);
    *p = num;
    return OK;
}

// 每个服务使用自己和frpc之间独立的长连接，来避免竞态条件
// 言外之意，这里的connection使用frpc主动连接过来的
RET_CODE Service::write_to_frpc(int length){
    int bytes_write = 0;
    int buffer_idx = 0;
    // 必须要要一次性发送成功
    while(true){
        if(buffer_idx>=length){
            memeset(buffet, '\0', length);
            return BUFFER_EMPTY;
        }
        int retry=0;
        label:
        bytes_write = send(connection, buffer+buffer_idx, length-buffer_idx, MSG_NOSIGNAL);
        if(bytes_write==-1){
            if(errno==EAGAIN || errno==EWOULDBLOCK){
                ++retry;
                if(retry>=5) return TRY_AGAIN;
            }
            return IOERR;
        }
        else if(bytes_write==0) return CLOSED;
        buffer_idx += bytes_write;
    }
    return OK;
}