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

#include <iostream>
#include <queue>
#include <unordered_map>
using namespace std;

// 描述frps主线程处理的事情，包含port侦听、与frpc通信开启remote_port、
// 侦听remote_port、向frpc请求建立连接 管理来自remote_port的连接和
// 请求建立的连接

class Master{
private:
    // 缓冲区
    static const int BUFFER_SIZE;
    static const int EVENTS_SIZE;
    char* buffer;
    int buffer_idx;

    // 连接socket相关
    int port;
    int listenfd, connection; // 和frpc的主连接 需要关闭的资源
    struct sockaddr_in addr, frpc_addr;
    int length;

    // 服务socket相关
    int serv_port;
    int serv_listenfd; // 需要关闭的资源
    struct sockadr_in serv_addr, client_addr;
    int serv_length;

    //epoll侦听
    int epollfd;
    epoll_event* events;

    // 连接管理
    queue<int>clients;
    queue<int>frpcs;

    // 功能
    void listen();
    void service_listen(int service_port);
    void connection_request();

    // 读写缓冲
    RET_CODE read_from_frpc();
    RET_CODE write_to_frpc(unsigned int);

    // 需要的连接数
    unsigned int conn_need;
public:
    Master(int port);
    ~Master();
    void start();
}

const int BUFFER_SIZE = 512;
const int EVENTS_SIZE = 5;

Master::Master(listen_port): port(listen_port), connection(-1), buffer_idx(0), conn_need(0){
    buffer = new char[BUFFER_SIZE];
    epollfd = epoll_create(1);
    events = new epoll_event[EVENTS_SIZE];
}

Master::~Master(){
    delete []buffer;
    delete []events;
    close(epollfd);
    close(listenfd);
    close(serv_listenfd);
}

RET_CODE Master::read_from_frpc(){
    // 缓冲区复用 每次读之前需要清空缓冲区
    memset(buffer, '\0', BUFFER_SIZE);
    int bytes_read = 0;
    while(true){
        // 出现这种情况需要报错 每次读缓冲区是全部空闲一定够用
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
    return (buffer_idx>0)? buffer_idx=0, OK: NOTHING;
}

RET_CODE Master::write_to_frpc(unsigned int num){
    // 每次发送一个字节，不能成功下次接着继续
    int length = sizeof(num);
    memset(buffer, '\0', BUFFER_SIZE);
    *((unsigned int*)buffer)=num;
    int bytes_write = 0;
    while(true){
        if(buffer_idx>=length){
            buffer_idx=0;
            return BUFFER_EMPTY;
        }
        bytes_write = send(connection, buffer+buffer_idx, length-buffer_idx, 0);
        if(bytes_write==-1){
            if(errno==AGAIN || errno==EWOULDBLOCK) return TRY_AGAIN;
            return IOERR;
        }
        else if(bytes_write==0) return CLOSED;
        buffer_idx+=bytes_write;
    }
    return OK;
}

void Master::start(){
    listen();
    length = sizeof(frpc_addr);
    socklen_t socklen = sizeof(frpc_addr);
    int num;
    RET_CODE res;
    bool stop=false;
    while(!stop){
        num = epoll_wait(epollfd, events, EVENTS_SIZE, -1);
        if(num==-1){
            perror("epoll_wait failed!");
            exit(1);
        }
        for(int i=0;i<num;++i){
            if(events[i].data.fd == listenfd && events[i].events | EPOLLIN){
                int conn = accept(listenfd, (struct sockaddr*)&frpc_addr, &socklen);
                if(conn==-1){
                    perror("accept failed!");
                    exit(1);
                }
                // connection还没赋值则构建frpc和frps的主连接
                if(connection==-1){
                    connection = conn;
                    add_readfd(epollfd, connection);
                }else{ // 是frpc的给其他服务的连接
                    frpcs.push(conn);
                }
            }
            // client连接
            else if(events[i].data.fd == serv_listenfd && events[i].events | EPOLLIN){
                int client_fd = accpet(serv_listenfd, (struct sockaddr*)&client_addr, &length);
                if(client_fd==-1){
                    perror("accept serv failed!");
                    exit(1);
                }
                clients.push(client_fd);
                // 准备发数据给frpc
                modfd(epollfd, connection, EPOLLOUT);
                ++conn_need;
            }
            // 读取frpc发送的配置数据 目前只启动一个服务（端口）
            else if(events[i].data.fd == connection && events[i].events | EPOLLIN){
                res = read_from_frpc();
                switch(res){
                    case BUFFER_FULL:{
                        perror("the buffer is not enough!");
                        stop=true;
                        break;
                    }
                    case IOERR:
                    case CLOSED:{
                        // 需要去关闭启动的服务和相关连接
                        close_fd(epollfd, serv_listenfd);
                        close_fd(epollfd, connection);
                        perror("the frpc closed or error!");
                        break;
                    }
                    default:
                        break;
                }
                serv_port = *((short*)buffer);
                service_listen(serv_port);
            }
            // frps请求frpc发起请求
            else if(events[i].data.fd == connection && events[i].events | EPOLLOUT){
                res = write_to_frpc(conn_need); // 每次需要发送一个unsigned int来代表需要的连接数
                // 必须要发送成功
                while(res==TRY_AGAIN){
                    res = write_to_frpc(conn_need);
                }
                conn_need = 0；
                switch(res){
                    case IOERR:
                    case CLOSED:{
                        close_fd(epollfd, serv_listenfd);
                        close_fd(epollfd, connection);
                        perror("the frpc closed or error!");
                        break;
                    }
                    default:
                        break;
                }
            }
        }

        // 分配子线程来服务， 设置分离态线程
        pthread_t tid;
        if(frpcs.size()>clients.size()){
            perror("the frpc connection is greater than client connection!");
            stop=true;
            continue;
        }
        while(!frpcs.empty()){
            int frpc_conn = frpcs.front(), client_conn = clients.front();
            frpcs.pop();
            clients.pop();
            Manager* manager = new Manager(client_conn, frpc_conn);
            ret=pthread_create(&tid, nullptr, Manager::start_routine, (void*)manager);
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
        // 当线程id不存在时回收资源 线程结束时自己去回收manager资源
        // 线程自己的资源通过设置分离态自己回收
    }
}

void Master::listen(){
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if(listenfd<0){
        perror("create listen socket failed!");
        exit(1);
    }
    int opt=1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void*)&opt, sizeof(opt));

    int ret=bind(listenfd, (struct sockaddr*)&addr, sizeof(addr));
    if(ret==-1){
        perror("bind failed!");
        exit(1);
    }
    ret = listen(listenfd, 5);
    if(ret==-1){
        perror("listen failed!");
        exit(1);
    }
    add_readfd(epollfd, listenfd);
}

// 如果请求该服务的frpc关闭，那么这项服务的侦听是否需要关闭？
void Master::service_listen(int service_port){
    bzero(&serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(service_port);

    serv_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if(serv_listenfd<0){
        perror("create serv listen socket failed!");
        exit(1);
    }
    
    int ret=bind(serv_listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    if(ret==-1){
        perror("bind service port failed!");
        exit(1);
    }
    add_readfd(serv_listenfd);  
}