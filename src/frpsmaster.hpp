#pragma once

#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/inet.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "util.hpp"
#include "manager.hpp"

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
    int read_idx;

    // 连接socket相关    
    int port;
    int listenfd, connection; // 和frpc的主连接
    struct sockaddr_in addr, frpc_addr;
    int length;

    // 服务socket相关
    int serv_port;
    int serv_listenfd;
    struct sockadr_in serv_addr, client_addr;
    int serv_length;

    //epoll侦听
    int epollfd;
    epoll_event* events;

    // 连接管理
    queue<int>clients;
    queue<int>frpcs;

    // 需要回收给每个线程分配的manager资源
    unordered_map<pthread_t, Manager*>resources;

    // 功能
    void listen();
    void service_listen(int service_port);
    void connection_request();
public:
    Master(int port);
    ~Master();
    void start();
}

const int BUFFER_SIZE = 512;
const int EVENTS_SIZE = 5;

Master::Master(listen_port): port(listen_port), connection(-1), read_idx(0){
    buffer = new char[BUFFER_SIZE];
    epollfd = epoll_clt(1);
    events = new epoll_event[EVENTS_SIZE];
}

Master::~Master(){
    delete []buffer;
    delete []events;
    close(epollfd);
    close(listenfd);
}

void Master::start(){
    listen();
    length = sizeof(frpc_addr);
    int num, ret;
    while(true){
        num = epoll_wait(epollfd, events, EVENTS_SIZE, -1);
        if(num==-1){
            perror("epoll_wait failed!");
            exit(1);
        }
        for(int i=0;i<num;++i){
            if(events[i].data.fd == listenfd && events[i].events | EPOLLIN){
                int conn = accept(listenfd, (struct sockaddr*)&frpc_addr, &length);
                if(conn)==-1){
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
                add_writefd(epollfd, connection);
            }
            // frpc发送配置数据 目前只启动一个服务（端口）
            else if(events[i].data.fd == connection && events[i].events | EPOLLIN){
                while(read_idx<BUFFER_SIZE){
                    ret = read_buffer(connection, buffer+read_idx, BUFFER_SIZE-read_idx);
                    if(ret==-1) break; // 读取完毕，退出
                    else if(ret>0) read_idx+=ret;
                    else if(ret==0||ret==-2){ //关闭连接 只管理自己，至于每个服务则有他们自己感知
                        close_fd(epollfd, connection);
                        break;
                    }
                }
                read_idx = 0;
                serv_port = *((int*)buffer);
                service_listen(serv_port);
            }
            // frps请求frpc发起请求
            else if(events[i].data.fd == connection && events[i].events | EPOLLOUT){
                memset(buffer, '\0', BUFFER_SIZE);
                *((int*)buffer)=1;
                while(read_idx<sizeof(int)){
                    ret = write_buffer(connection, buffer+read_idx, sizeof(int)-read_idx);
                    if(ret==-1) break; // 停止发送
                    else if(ret>0) read_idx += ret;
                    else if(ret==0||ret==-2){ // 关闭连接
                        close_fd(epollfd, connection);
                        break;
                    }
                }
                read_idx=0;
            }
        }

        // 分配子线程来服务， 设置分离态线程
        pthread_t tid;
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
            re=pthread_detach(tid);
            if(ret!=0){
                perror("pthread_detach failed!");
                exit(1);
            }
            resources[tid]=manager;
        }

        // 当线程id不存在时回收资源
        for(auto iter: resources){
            pthread_t tid = iter->first;
            Manager* manager = iter->second;
            ret = pthread_kill(tid,0);
            if(ret==ESRCH){
                delete manager;
            }else if(ret==EINVAL){
                perror("the pthread_kill signal is not legal!");
                exit(1);
            }
        }
    }
    pthread_exit(nullptr);
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
    setsockopt(listenfd, SOL_SOCKET, (const void*)&opt, sizeof(opt));

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

// 如果请求该服务的frpc关闭，那么这项服务的侦听需要关闭
void service_listen(int service_port){
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