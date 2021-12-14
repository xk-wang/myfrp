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
#include "frpsmanager.hpp"

#include <iostream>
#include <queue>
#include <unordered_map>
#include <vector>
using namespace std;

// 现在逻辑控制如下：
// frps端的listenfd和服务逻辑都交给主线程去控制
// frpc端发来的连接开始不进行具体服务分配，是通用连接
// 同时frpc对这些连接进行侦听
// s端拿到这些连接后将连接分配给具体的服务线程
// 服务线程接受了之后第一步先向c端发送端口，并等待c端返回ok
// c端接收到端口之后和本地的端口建立连接同时发送OK回去
// c端和s端的服务线程才开启

// 主线程的心跳逻辑保持不变

class Master{
private:
    static const int BUFFER_SIZE;
    static const int EVENTS_SIZE;
    static const int TIME_OUT;
    static const int MAX_HEART_BEATS;

    char* buffer;

    // 连接socket相关
    int listenfd, connection; // 和frpc的主连接 需要关闭的资源
    short port;

    // 服务socket相关
    vector<int>serv_listenfds;

    //epoll侦听
    int epollfd;
    
    // 功能
    int Listen(short listen_port);
    void connection_request();

    // 读写缓冲
    RET_CODE recv_port(vector<short>&serv_ports);
    int read_ports_from_frpc();
    RET_CODE write_to_frpc();
    RET_CODE write_to_buffer(int num);
    short read_from_buffer();
    
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

Master::Master(short listen_port): port(listen_port), connection(-1){
    buffer = new char[BUFFER_SIZE];
    epollfd = epoll_create(1);
}

Master::~Master(){
    delete []buffer;
    close_file(epollfd);
    close_file(listenfd);
    for(int serv_listenfd: serv_listenfds) close_file(serv_listenfd);
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
    listenfd=Listen(port);
    epoll_event events[EVENTS_SIZE];
    struct sockaddr_in frpc_addr, client_addr;
    socklen_t socklen = sizeof(frpc_addr);

    // 需要的连接数
    int conn_need=0;
    int heart_count=0;

    int num;
    RET_CODE res;
    bool stop=false;
    
    clock_t start_time = clock();
    // 建立listenfd和port的映射
    unordered_map<int, short>maps;
    // 保存端口和连接
    queue<pair<short, int>>clients;
    queue<int>frpcs;

    // 保存端口
    vector<short>serv_ports;
    
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
                close_fd(epollfd, connection);
                for(int serv_listenfd: serv_listenfds) close_file(serv_listenfd);
                serv_listenfds.clear();
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
            // 读取frpc发送的配置数据 目前只启动一个服务（端口）
            else if(events[i].data.fd == connection && (events[i].events & EPOLLIN)){
                start_time = clock();
                heart_count = 0;
                // 读取端口号
                res = recv_port(serv_ports);
                switch(res){
                    case(-1):{
                        LOG(INFO) << "read the connection failed!";
                        close_fd(epollfd, connection);
                        for(int serv_listenfd: serv_listenfds) close_file(serv_listenfd);
                        serv_listenfds.clear();
                        connection=-1;
                        break;
                    }
                    case(0):{
                        for(short port: serv_ports){
                            int fd = Listen(port);
                            maps[fd] = port;
                        }
                    }
                    case(1):{
                        LOG(INFO) << "receive heart beat pack from frpc";
                        break;
                    }
                    default:
                        break;
                }
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
                        close_fd(epollfd, connection);
                        for(int serv_listenfd: serv_listenfds) close_file(serv_listenfd);
                        connection = -1;
                        LOG(ERROR) << "the frpc error!";
                        break;
                    }
                    case CLOSED:{
                        close_fd(epollfd, connection);
                        for(int serv_listenfd: serv_listenfds) close_file(serv_listenfd);
                        connection = -1;
                        LOG(ERROR) << "the frpc closed!";
                        break;
                    }
                    case TRY_AGAIN:{
                        close_fd(epollfd, connection);
                        for(int serv_listenfd: serv_listenfds) close_file(serv_listenfd);
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
            // client连接
            else if(events[i].events & EPOLLIN){
                for(auto iter: maps){
                    int fd = iter.first;
                    short port = iter.second;
                    if(fd==events[i].data.fd){
                        int client_fd = accept(fd, (struct sockaddr*)&client_addr, &socklen);
                        if(client_fd==-1){
                            perror("accept serv failed!");
                            exit(1);
                        }
                        LOG(INFO) << "client: " << client_fd << " connect";
                        clients.push({port, client_fd});
                        // 准备发数据给frpc
                        modfd(epollfd, connection, EPOLLOUT);
                        ++conn_need;
                        LOG(INFO) << "the conn_need: " << conn_need;
                    }
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
            int frpc_conn = frpcs.front();
            auto client = clients.front();
            frpcs.pop();
            clients.pop();
            FPRSManager* manager = new FPRSManager(client.first, client.second, frpc_conn);
            int ret=pthread_create(&tid, nullptr, FPRSManager::start_frps_routine, (void*)manager);
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


// -1出错 0 frpc发送的配置连接 1心跳包
int Master::recv_port(vector<short>& serv_ports){
    RET_CODE res = read_ports_from_frpc();
    switch(res){
        case IOERR:{
            LOG(ERROR) << "the frpc error";
            return -1;
        }
        case CLOSED:{
            LOG(ERROR) << "the frpc closed";
            return -1;
        }
        case BUFFER_FULL:{
            LOG(ERROR) << "the buffer is not enough to save ports";
            return -1;
        }
        case NOTHING:{
            LOG(ERROR) << "the frps read nothing";
            return -1;
        }
        default:
            break;
    }
    short* p = buffer;
    short num = *p++;
    if(num==-2) return 1;
    for(int i=0;i<num;++i){
        serv_ports.push_back(*p++);
    }
    memset(buffer, '\0', BUFFER_SIZE);
    return 0;
}

RET_CODE Master:: read_ports_from_frpc(){
    memset(buffer, '\0', BUFFER_SIZE);
    int bytes_read = 0, buffer_idx = 0;
    while(true){
        if(buffer_idx>BUFFER_SIZE){
            return BUFFER_FULL;
        }
        bytes_read = recv(connection, buffer+buffer_idx, 0);
        if(bytes_read==-1){
            if(errno==EAGAIN || errno==EWOULDBLOCK){
                break;
            }
            return IOERR;
        }
        else if(bytes_read=0) return CLOSED;
        buffer_idx += bytes_read;
    }
    return buffer_idx>0? OK: NOTHING; 
}

int Master::Listen(short listen_port){
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    // inet_pton(AF_INET, "192.168.66.16", &addr.sin_addr);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(listen_port);
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
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
    LOG(INFO) << "the frps listening: " << ntohs(addr.sin_port);
    add_readfd(epollfd, listenfd);
    return listenfd;
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
    LOG(INFO) << "the service at " << service_port << " listening";
    add_readfd(epollfd ,serv_listenfd);  
}

double Master::interval(){
    clock()-start_time;
    return (double)(clock()-start_time)*1000/CLOCKS_PER_SEC;
}

// frps和frpc只有两种类型的交互, 向frps发配置文件，向frpc发请求
// 每次交互必须要先完成才能进行之后的动作

// 接收一个端口号或者心跳包
RET_CODE Master::read_from_frpc(){
    int buffer_idx = 0;
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
    return (buffer_idx>0)? OK: NOTHING;
}

// 发送一个int
RET_CODE Master::write_to_frpc(){
    int bytes_write = 0, length = sizeof(int);
    int buffer_idx = 0;
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