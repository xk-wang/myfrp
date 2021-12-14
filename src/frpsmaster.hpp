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

// 功能：
// 1.serv_port启动服务
// 2.接收frpc连接connection
// 3.接收frpc的端口配置
// 4.开启服务端口侦听且进行监听，建立侦听和remote_port的映射
// 5.接收客户端连接，保存客户端的连接和remote_port
// 6.通过connection向frpc发送需求
// 7.接收frpc的主动连接，分配给具体的客户端
// 8.构建工作线程(正式工作前向frpc发送remote_port，同时等待frpc发送ok状态码)
// 9.监控connection的心跳

class Master{
private:
    // 常量
    static const int BUFFER_SIZE;
    static const int EVENTS_SIZE;
    static const int TIME_OUT;
    static const int MAX_HEART_BEATS;

    // 缓冲区资源
    char* buffer;

    // frps侦听
    int listenfd;
    // 和frpc的固定连接
    int connection;
    // 本地侦听的端口
    short port;

    // 具体服务的侦听文件描述符
    vector<int>serv_listenfds;

    //epoll监听
    int epollfd;
    
    // 功能
    int Listen(short listen_port);
    int recv_port(vector<short>&serv_ports);
    int send_request(short conn_need);

    // 底层读写
    RET_CODE read_from_frpc();
    RET_CODE write_to_frpc(int length);
    
    double interval(clock_t start_time);
    
public:
    Master(short listen_port);
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

// 版本1出错日志，服务端没发现客户端掉线，因此客户端重新连接被认定是发来新的connection用来
// 给service服务

// the frps listening: 20211
// frpc connect
// the service at: 20211 listening
// start service at port: 32319
// get conn for service
// the frpc connection is greater than client connection!

void Master::start(){
    listenfd=Listen(port);
    if(listenfd==-1) return;
    epoll_event events[EVENTS_SIZE];
    struct sockaddr_in frpc_addr, client_addr;
    socklen_t socklen = sizeof(frpc_addr);

    // 需要的连接数
    short conn_need=0;
    int heart_count=0;

    int num, ret;
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
            LOG(ERROR) <<"epoll_wait failed!";
            break;
        }
        if(interval(start_time)>=TIME_OUT){
            ++heart_count;
            if(heart_count>=MAX_HEART_BEATS){
                LOG(ERROR) << "the connection closed or error!";
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
                    LOG(ERROR) << "accept failed!";
                    continue;                 
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
            // 读取frpc发送的配置数据
            else if(events[i].data.fd == connection && (events[i].events & EPOLLIN)){
                start_time = clock();
                heart_count = 0;
                // 读取端口号
                ret = recv_port(serv_ports);
                switch(ret){
                    case(-1):{
                        close_fd(epollfd, connection);
                        for(int serv_listenfd: serv_listenfds) close_file(serv_listenfd);
                        serv_listenfds.clear();
                        connection=-1;
                        break;
                    }
                    case(0):{
                        for(short port: serv_ports){
                            int fd = Listen(port);
                            if(fd==-1){
                                LOG(ERROR) << "the service start failed";
                                continue;
                            }
                            maps[fd] = port;
                        }
                        break;
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
                ret = send_request(conn_need);
                if(ret==-1){
                    stop=true;
                    break;
                }
                conn_need = 0;
                modfd(epollfd, connection, EPOLLIN);
                LOG(INFO) << "already write the need connection to frpc";
            }
            // client连接
            else if(events[i].events & EPOLLIN){
                int fd = events[i].data.fd;
                if(maps.find(fd)==maps.end()){
                    LOG(ERROR) << "wrong epollin event";
                    continue;
                }
                short port = maps[fd];
                int client_fd = accept(fd, (struct sockaddr*)&client_addr, &socklen);
                if(client_fd==-1){
                    LOG(ERROR) << "accept serv failed!";
                    continue;
                }
                LOG(INFO) << "client: " << client_fd << " connect";
                clients.push({port, client_fd});
                // 准备发数据给frpc
                modfd(epollfd, connection, EPOLLOUT);
                ++conn_need;
                LOG(INFO) << "the conn_need: " << conn_need;     
            }
        }

        // 分配子线程来服务， 设置分离态线程
        pthread_t tid;
        if(frpcs.size()>clients.size()){
            LOG(ERROR) << "the frpc connection is greater than client connection!";
            LOG(ERROR) << "the frpc size: " << frpcs.size() 
                       << "the clients size: " << clients.size();
            break;
        }
        while(!frpcs.empty()){
            int frpc_conn = frpcs.front();
            auto client = clients.front();
            frpcs.pop();
            clients.pop();
            FRPSManager* manager = new FRPSManager(client.first, client.second, frpc_conn);
            int ret=pthread_create(&tid, nullptr, FRPSManager::start_frps_routine, (void*)manager);
            if(ret!=0){
                LOG(ERROR) <<"pthread_create failed!";
                close_file(frpc_conn);
                close_file(client.second);
                continue;
            }
            ret=pthread_detach(tid);
            if(ret!=0){
                LOG(ERROR) <<"pthread_detach failed!";
                close_file(frpc_conn);
                close_file(client.second);
            }
        }
    }
}


// -1出错 0 frpc发送的配置连接 1心跳包
int Master::recv_port(vector<short>& serv_ports){
    RET_CODE res = read_from_frpc();
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
    short* p = (short*)buffer;
    short num = *p++;
    if(num==-2) return 1;
    for(int i=0;i<num;++i){
        serv_ports.push_back(*p++);
    }
    return 0;
}

int Master::send_request(short conn_need){
    if(BUFFER_SIZE<sizeof(conn_need)){
        LOG(ERROR) << "the buffer is not enough to send conn_need";
        return -1;
    }
    *((short*)buffer)=conn_need;

    RET_CODE res=write_to_frpc(sizeof(short));
    switch(res){
        case IOERR:{
            LOG(ERROR) << "the frpc error";
            return -1;
        }
        case CLOSED:{
            LOG(ERROR) << "the frpc closed";
            return -1;
        }
        case TRY_AGAIN:{
            LOG(ERROR) << "the kernel is not enough to send conn_need";
            return -1;
        }
        default:
            break;
    }
    return 0;
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
        LOG(ERROR) << "create listen socket failed!";
        return -1;
    }
    // int opt=1;
    // setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void*)&opt, sizeof(opt));

    int ret=bind(listenfd, (struct sockaddr*)&addr, sizeof(addr));
    if(ret==-1){
        LOG(ERROR) << "bind failed!";
        return -1;
    }
    ret = listen(listenfd, 5);
    if(ret==-1){
        LOG(ERROR) << "listen failed!";
        return -1;
    }
    LOG(INFO) << "the frps listening: " << ntohs(addr.sin_port);
    add_readfd(epollfd, listenfd);
    return listenfd;
}

double Master::interval(clock_t start_time){
    return (double)(clock()-start_time)*1000/CLOCKS_PER_SEC;
}

RET_CODE Master:: read_from_frpc(){
    int bytes_read, buffer_idx = 0;
    while(true){
        if(buffer_idx>=BUFFER_SIZE){
            return BUFFER_FULL;
        }
        bytes_read = recv(connection, buffer+buffer_idx, BUFFER_SIZE-buffer_idx, 0);
        if(bytes_read==-1){
            if(errno==EAGAIN || errno==EWOULDBLOCK){
                break;
            }
            return IOERR;
        }
        else if(bytes_read==0) return CLOSED;
        buffer_idx += bytes_read;
    }
    return buffer_idx>0? OK: NOTHING; 
}

// 发送一个int
RET_CODE Master::write_to_frpc(int length){
    int bytes_write = 0;
    int buffer_idx = 0;
    while(true){
        if(buffer_idx>=length){
            return BUFFER_EMPTY;
        }
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