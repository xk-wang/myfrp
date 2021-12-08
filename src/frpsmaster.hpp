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
using namespace std;

// 描述frps主线程处理的事情，包含port侦听、与frpc通信开启remote_port、
// 侦听remote_port、向frpc请求建立连接 管理来自remote_port的连接和
// 请求建立的连接

class Master{
private:
    // 也需要设计两个缓冲区，现在一个缓冲区每次读写之前需要进行memset需要消息一次发送
    // 并不合理
    // 缓冲区
    static const int BUFFER_SIZE;
    static const int EVENTS_SIZE;
    char* buffer;
    int buffer_idx;

    // 连接socket相关
    int port;
    int listenfd, connection; // 和frpc的主连接 需要关闭的资源
    struct sockaddr_in addr, frpc_addr;
    socklen_t length;

    // 服务socket相关
    short serv_port;
    int serv_listenfd; // 需要关闭的资源
    struct sockaddr_in serv_addr, client_addr;
    int serv_length;

    //epoll侦听
    int epollfd;

    // 连接管理
    queue<int>clients;
    queue<int>frpcs;

    // 功能
    void Listen();
    void service_listen(short service_port);
    void connection_request();

    // 读写缓冲
    RET_CODE read_from_frpc();
    RET_CODE write_to_frpc();
    RET_CODE write_to_buffer(int num);
    short read_from_buffer();

    // 需要的连接数
    unsigned int conn_need;
public:
    Master(int listen_port);
    ~Master();
    void start();
};

const int Master::BUFFER_SIZE = 512;
const int Master::EVENTS_SIZE = 5;

Master::Master(int listen_port): port(listen_port), connection(-1), buffer_idx(0), conn_need(0){
    buffer = new char[BUFFER_SIZE];
    epollfd = epoll_create(1);

}

Master::~Master(){
    delete []buffer;
    close(epollfd);
    close(listenfd);
    close(serv_listenfd);
}

// frps和frpc只有两种类型的交互, 向frps发配置文件，向frpc发请求
// 每次交互必须要先完成才能进行之后的动作

// 读取配置文件只会发送一次(仅仅是发送一个端口号)
RET_CODE Master::read_from_frpc(){
    buffer_idx = 0;
    memset(buffer, '\0', BUFFER_SIZE);
    int bytes_read = 0;
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

// 发送一个int
RET_CODE Master::write_to_frpc(){
    int bytes_write = 0, length = sizeof(int);
    buffer_idx = 0;
    while(true){
        if(buffer_idx>=length){
            buffer_idx = 0;
            memset(buffer, '\0', BUFFER_SIZE);
            return BUFFER_EMPTY;
        }
        bytes_write = send(connection, buffer+buffer_idx, length-buffer_idx, 0);
        if(bytes_write==-1){
            if(errno==EAGAIN || errno==EWOULDBLOCK){
                return TRY_AGAIN;
            }
            return IOERR;
        }
        else if(bytes_write==0) return CLOSED;
        buffer_idx+=bytes_write;
    }
    return OK;
}

// 如果不能放下一个int需要报错
RET_CODE Master::write_to_buffer(int num){
    if(BUFFER_SIZE<sizeof(num)){
        return BUFFER_FULL;
    }
    memset(buffer, '\0', BUFFER_SIZE);
    *((int*)buffer)=num;
    return OK;
}

short Master::read_from_buffer(){
    short port = *((short*)buffer);
    memset(buffer, '\0', BUFFER_SIZE);
    return port;
}

void Master::start(){
    Listen();
    length = sizeof(frpc_addr);
    socklen_t socklen = sizeof(frpc_addr);
    int num;
    RET_CODE res;
    bool stop=false;
    epoll_event events[EVENTS_SIZE];
    while(!stop){
        num = epoll_wait(epollfd, events, EVENTS_SIZE, -1);
        if(num==-1){
            perror("epoll_wait failed!");
            exit(1);
        }
        for(int i=0;i<num;++i){
            if(events[i].data.fd == listenfd && (events[i].events & EPOLLIN)){
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
            else if(events[i].data.fd == serv_listenfd && (events[i].events & EPOLLIN)){
                int client_fd = accept(serv_listenfd, (struct sockaddr*)&client_addr, &length);
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
            else if(events[i].data.fd == connection && (events[i].events & EPOLLIN)){
                res = read_from_frpc();
                switch(res){
                    case BUFFER_FULL:{
                        cout << "the buffer is not enough!" << endl;
                        stop=true;
                        continue;
                    }
                    case IOERR:
                    case CLOSED:
                    case NOTHING: {
                        // 需要去关闭启动的服务和相关连接
                        close_fd(epollfd, serv_listenfd);
                        close_fd(epollfd, connection);
                        cout << "the frpc closed or error or send nothing!" << endl;
                        break;
                    }
                    default:
                        break;
                }
                serv_port = read_from_buffer();
                service_listen(serv_port);
            }
            // frps请求frpc发起请求
            else if(events[i].data.fd == connection && (events[i].events & EPOLLOUT)){
                if(conn_need<=0){
                    cout << "the conn_need is not positive" << endl;
                    modfd(epollfd, connection, EPOLLIN);
                    continue;
                }
                res = write_to_buffer(conn_need);
                if(res==BUFFER_FULL){
                    cout << "the buffer is not enough" << endl;
                    stop=true;
                    continue;
                }
                res = write_to_frpc(); // 每次需要发送一个unsigned int来代表需要的连接数
                switch(res){
                    case IOERR:
                    case CLOSED:
                    case TRY_AGAIN:{
                        close_fd(epollfd, serv_listenfd);
                        close_fd(epollfd, connection);
                        cout << "the frpc closed or error or the kernel is not enough to write!" << endl;
                        break;
                    }
                    default:
                        break;
                }
                conn_need = 0;
                modfd(epollfd, connection, EPOLLIN);
            }
        }

        // 分配子线程来服务， 设置分离态线程
        pthread_t tid;
        if(frpcs.size()>clients.size()){
            cout << "the frpc connection is greater than client connection!" <<endl;
            stop=true;
            continue;
        }
        while(!frpcs.empty()){
            int frpc_conn = frpcs.front(), client_conn = clients.front();
            frpcs.pop();
            clients.pop();
            Manager* manager = new Manager(client_conn, frpc_conn);
            int ret=pthread_create(&tid, nullptr, Manager::start_routine, (void*)manager);
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

void Master::Listen(){
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
void Master::service_listen(short service_port){
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
    add_readfd(epollfd ,serv_listenfd);  
}