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
#include <time.h>

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
    static const int TIME_OUT;
    static const int MAX_HEART_BEATS;

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

    // 心跳包
    clock_t start_time;
    int heart_count;
    double interval();
public:
    Master(int listen_port);
    ~Master();
    void start();
};

const int Master::BUFFER_SIZE = 512;
const int Master::EVENTS_SIZE = 5;
const int Master::TIME_OUT = 40000;
const int Master::MAX_HEART_BEATS = 3;

Master::Master(int listen_port): port(listen_port), connection(-1), buffer_idx(0), conn_need(0),
                                 heart_count(0){
    buffer = new char[BUFFER_SIZE];
    epollfd = epoll_create(1);

}

Master::~Master(){
    delete []buffer;
    close(epollfd);
    close(listenfd);
    close(serv_listenfd);
}

double Master::interval(){
    clock()-start_time;
    return (double)(clock()-start_time)*1000/CLOCKS_PER_SEC;
}

// frps和frpc只有两种类型的交互, 向frps发配置文件，向frpc发请求
// 每次交互必须要先完成才能进行之后的动作

// 接收一个端口号或者心跳包
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
        // 防止连接关闭程序发生崩溃
        bytes_write = send(connection, buffer+buffer_idx, length-buffer_idx, MSG_NOSIGNAL);
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

// 出错日志，服务端没发现客户端掉线，因此客户端重新连接被认定是发来新的connection用来
// 给service服务

// the frps listening: 20211
// frpc connect
// the service at: 20211 listening
// start service at port: 32319
// get conn for service
// the frpc connection is greater than client connection!

// 添加心跳包来保持frpc和frps的长连接
// 基本原理是frpc通过长连接connection每隔30s向frps发送心跳包，如果connection断开那么
// 心跳包会发送失败，产生ioerr, frpc关闭
// frps也设置超时参数，至少timeout内需要收到心跳包才算正常, 否则代表连接失效
void Master::start(){
    Listen();
    length = sizeof(frpc_addr);
    socklen_t socklen = sizeof(frpc_addr);
    int num;
    RET_CODE res;
    bool stop=false;
    epoll_event events[EVENTS_SIZE];
    start_time = clock();
    while(!stop){
        // 有连接事件不代表存在connection事件或者心跳包，listenfd的不算
        num = epoll_wait(epollfd, events, EVENTS_SIZE, TIME_OUT);
        if(num==-1){
            perror("epoll_wait failed!");
            exit(1);
        }
        if(interval()>=TIME_OUT){
            ++heart_count;
            if(heart_count>=MAX_HEART_BEATS){
                LOG(INFO) << "the connection closed or error!";
                close_fd(epollfd, serv_listenfd);
                close_fd(epollfd, connection);
                connection=-1;
            }
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
                    LOG(INFO) << "frpc connect";
                    connection = conn;
                    add_readfd(epollfd, connection);
                }else{ // 是frpc的给其他服务的连接
                    LOG(INFO) << "get conn for service";
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
                LOG(INFO) << "client: " << client_fd << " connect";
                clients.push(client_fd);
                // 准备发数据给frpc
                modfd(epollfd, connection, EPOLLOUT);
                ++conn_need;
                LOG(INFO) << "the conn_need: " << conn_need;
            }
            // 读取frpc发送的配置数据 目前只启动一个服务（端口）
            else if(events[i].data.fd == connection && (events[i].events & EPOLLIN)){
                start_time = clock();
                heart_count = 0;
                res = read_from_frpc();
                switch(res){
                    case BUFFER_FULL:{
                        LOG(ERROR) << "the buffer is not enough!";
                        close_fd(epollfd, serv_listenfd);
                        close_fd(epollfd, connection);
                        connection=-1;
                        stop=true;
                        break;
                    }
                    case IOERR:{
                        LOG(ERROR) << "the frpc error!";
                        close_fd(epollfd, serv_listenfd);
                        close_fd(epollfd, connection);
                        connection=-1;
                        break;
                    }
                    case CLOSED:{
                        LOG(ERROR) << "the frpc closed!";
                        close_fd(epollfd, serv_listenfd);
                        close_fd(epollfd, connection);
                        connection=-1;
                        break;
                    }
                    case NOTHING: {
                        // 需要去关闭启动的服务和相关连接
                        close_fd(epollfd, serv_listenfd);
                        close_fd(epollfd, connection);
                        connection=-1;
                        LOG(ERROR) << "the frpc send nothing!";
                        break;
                    }
                    case OK: {
                        // 读取端口信息 或者 心跳包
                        short value = read_from_buffer();
                        if(value==short(-2)){
                            LOG(INFO) << "receive heart beat pack from frpc";
                        }
                        else{
                            serv_port = value;
                            service_listen(serv_port);
                        }
                    }
                    default:
                        break;
                }
                if(stop) break;

            }
            // frps请求frpc发起请求
            else if(events[i].data.fd == connection && (events[i].events & EPOLLOUT)){
                start_time = clock();
                heart_count = 0;
                if(conn_need<=0){
                    LOG(ERROR) << "the conn_need is not positive";
                    modfd(epollfd, connection, EPOLLIN);
                    continue;
                }
                res = write_to_buffer(conn_need);
                if(res==BUFFER_FULL){
                    LOG(ERROR) << "the buffer is not enough";
                    stop=true;
                    break;
                }
                res = write_to_frpc(); // 每次需要发送一个unsigned int来代表需要的连接数
                switch(res){
                    case IOERR:{
                        close_fd(epollfd, serv_listenfd);
                        close_fd(epollfd, connection);
                        connection = -1;
                        LOG(ERROR) << "the frpc error!";
                        break;
                    }
                    case CLOSED:{
                        close_fd(epollfd, serv_listenfd);
                        close_fd(epollfd, connection);
                        connection = -1;
                        LOG(ERROR) << "the frpc closed!";
                        break;
                    }
                    case TRY_AGAIN:{
                        close_fd(epollfd, serv_listenfd);
                        close_fd(epollfd, connection);
                        connection = -1;
                        LOG(ERROR) << "the kernel is not enough to write!";
                        break;
                    }
                    case BUFFER_EMPTY:{
                        conn_need = 0;
                        modfd(epollfd, connection, EPOLLIN);
                        LOG(INFO) << "already write the need connection to frpc";
                    }
                    default:
                        break;
                }
            }
        }

        // 分配子线程来服务， 设置分离态线程
        pthread_t tid;
        if(frpcs.size()>clients.size()){
            LOG(ERROR) << "the frpc connection is greater than client connection!";
            LOG(ERROR) << "the frpc size: " << frpcs.size() 
                       << "the clients size: " << clients.size();
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
    // inet_pton(AF_INET, "192.168.66.16", &addr.sin_addr);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if(listenfd<0){
        perror("create listen socket failed!");
        exit(1);
    }
    // int opt=1;
    // setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void*)&opt, sizeof(opt));

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
    LOG(INFO) << "the frps listening: " << port;
    add_readfd(epollfd, listenfd);
}

// 如果请求该服务的frpc关闭，那么这项服务的侦听是否需要关闭？
void Master::service_listen(short service_port){
    bzero(&serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    // inet_pton(AF_INET, "192.168.66.16", &serv_addr.sin_addr);
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(service_port);

    serv_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if(serv_listenfd<0){
        perror("create serv listen socket failed!");
        exit(1);
    }
    // int opt=1;
    // setsockopt(serv_listenfd, SOL_SOCKET, SO_REUSEADDR, (const void*)&opt, sizeof(opt));
    
    int ret=bind(serv_listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    if(ret==-1){
        perror("bind service port failed!");
        exit(1);
    }
    ret = listen(serv_listenfd, 5);
    if(ret==-1){
        perror("listen failed!");
        exit(1);
    }
    LOG(INFO) << "the service at " << port << " listening";
    add_readfd(epollfd ,serv_listenfd);  
}