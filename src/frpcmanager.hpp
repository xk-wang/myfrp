#pragma once

#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <unordered_map>

#include "util.hpp"
#include "easylogging++.h"
using namespace std;

class FRPCManager{
private:
    // 常量设置
    static const int BIG_BUFFER_SIZE;
    static const int EVENTS_SIZE;

    // 缓冲区设置
    char *forward_buffer, *backward_buffer;
    int forward_read_idx, forward_write_idx;
    int backward_read_idx, backward_write_idx;

    // remote_port-->locals映射
    unordered_map<short, struct sockaddr_in>locals;

// 对象的公共成员
public:
    // frpc的工作线程
    FRPCManager(int ffd1, const unordered_map<short, struct sockaddr_in>&los);
    ~FRPCManager();
    // 侦听
    int epollfd;
    // 和frps建立的连接
    int fd1;
    // 和本地建立的连接
    int fd2;
    // 和本地服务建立连接
    int connect_local();
    // 发送状态给frps
    int send_state();

    // socket读写
    RET_CODE read_fd1();
    RET_CODE write_fd1();
    RET_CODE read_fd2();
    RET_CODE write_fd2();

// 类的公共方法
public:
    static void* start_frpc_routine(void* arg);
};

// 定义
const int FRPCManager::EVENTS_SIZE = 5;
const int FRPCManager::BIG_BUFFER_SIZE = 65535;

FRPCManager::FRPCManager(int ffd1, const unordered_map<short, struct sockaddr_in>&los):
        fd1(ffd1), fd2(-1), locals(los),
        forward_read_idx(0), forward_write_idx(0),
        backward_read_idx(0), backward_write_idx(0){
    forward_buffer = new char[BIG_BUFFER_SIZE];
    backward_buffer = new char[BIG_BUFFER_SIZE];
    epollfd = epoll_create(1);
}

FRPCManager::~FRPCManager(){
    delete []forward_buffer;
    delete []backward_buffer;
    close_file(fd1);
    close_file(fd2);
    close_file(epollfd);
}

void* FRPCManager::start_frpc_routine(void* arg){
    FRPCManager* manager = (FRPCManager*)arg;
    int epollfd = manager->epollfd;
    int fd1 = manager->fd1, fd2;
    epoll_event events[EVENTS_SIZE]; 
    add_readfd(epollfd, fd1);

    int ret, num;
    RET_CODE res;
    bool stop=false;
    // 首次侦听到读是发送过来的端口号
    // 首次侦听到写是发送状态
    bool comm_in=true, comm_out=true;
    while(!stop){
        int ret = epoll_wait(epollfd, events, EVENTS_SIZE, -1);
        if(ret==-1){
            perror("epoll_wait failed!");
            exit(1);
        }

        for(int i=0;i<ret;++i){
            //读取fd1数据 区分是开始通信还是后续的数据交互
            if(events[i].data.fd == fd1 && (events[i].events & EPOLLIN)){
                if(comm_in){
                    // 读取发送过来的端口号并且建立和本地的连接
                    fd2 = manager->connect_local();
                    manager->fd2 = fd2;
                    if(fd2==-1){
                        stop = true;
                        break;
                    }
                    //准备发送状态码
                    modfd(epollfd, fd1, EPOLLOUT);
                    comm_in = false;
                }
                else{
                    res = manager->read_fd1();
                    LOG(INFO) << "read from " << fd1;
                    switch(res){
                        case OK:
                        case BUFFER_FULL:{
                            modfd(epollfd, fd2, EPOLLOUT);
                            break;
                        }
                        case IOERR:
                        case CLOSED:{
                            stop=true;
                            break;
                        }
                        default:
                            break;
                    }
                }
            }
            // 发送给fd1端 区分通信开始或者是数据交互
            else if(events[i].data.fd == fd1 && (events[i].events & EPOLLOUT)){
                // 发送状态码
                if(comm_out){
                    int state = manager->send_state();
                    // 出错停掉连接
                    if(state==-1){
                        stop = true;
                        break;
                    }
                    // 开始数据交互
                    modfd(epollfd, fd1, EPOLLIN);
                    add_readfd(epollfd, fd2);
                    comm_out = false;
                }
                else{
                    res = manager->write_fd1();
                    LOG(INFO) << "write to " << fd1;
                    switch(res){
                        // 数据发送完毕 只改自己的状态为读侦听
                        case BUFFER_EMPTY:{
                            modfd(epollfd, fd1, EPOLLIN);
                            break;
                        }
                        // 数据还没完全发送完毕
                        case TRY_AGAIN:{
                            modfd(epollfd, fd1, EPOLLOUT);
                            break;
                        }
                        case IOERR:
                        case CLOSED:{
                            stop=true;
                            break;
                        }
                        default:
                            break;
                    }
                }
            }
            // 读取fd2数据
            else if(events[i].data.fd == fd2 && (events[i].events & EPOLLIN)){
                res = manager->read_fd2();
                LOG(INFO) << "read from " << fd2;
                switch(res){
                    case OK:
                    case BUFFER_FULL:{
                        modfd(epollfd, fd1, EPOLLOUT);
                        break;
                    }
                    case IOERR:
                    case CLOSED:{
                        stop=true;
                        break;
                    }
                    default:
                        break;
                }
            }
            // 发送给fd2端
            else if(events[i].data.fd == fd2 && (events[i].events & EPOLLOUT)){
                res = manager->write_fd2();
                LOG(INFO) << "write to " << fd2;
                switch(res){
                    case BUFFER_EMPTY:{
                        modfd(epollfd, fd2, EPOLLIN);
                        break;
                    }
                    case TRY_AGAIN:{
                        modfd(epollfd, fd2, EPOLLOUT);
                        break;
                    }
                    case IOERR:
                    case CLOSED:{
                        stop=true;
                        break;
                    }
                    default:
                        break;
                }
            }
            // 其他事件数据错误
            else{
                LOG(ERROR) <<"the event is is not right";
                stop=true;
            }
        }
    }
    delete manager;
    return nullptr;
}

int FRPCManager::connect_local(){
    // 从socket中读取端口
    int buffer_idx=0, length=sizeof(short);
    int bytes_read=0;
    char* buffer = new char[length];
    while(true){
        if(buffer_idx>length){
            LOG(ERROR) << "the buffer is not enough to save port";
            return -1;
        }
        bytes_read = recv(fd1, buffer+buffer_idx, length-buffer_idx, 0);
        if(bytes_read==-1){
            if(errno==EAGAIN || errno==EWOULDBLOCK) break;
            LOG(ERROR) << "the frps error";
            return -1;
        }
        else if(bytes_read==0){
            LOG(ERROR) << "the frps closed";
            return -1;
        }
        buffer_idx += bytes_read;
    }
    if(buffer_idx==0){
        LOG(ERROR) << "read port nothing from frps";
        return -1;
    }
    short remote_port = *(short*)buffer;
    delete []buffer;
    if(remote_port<0){
        LOG(ERROR) << "the remote port is negative";
        return -1;
    }
    
    // 建立和本地的连接 本地的地址提前弄好了，直接进行使用即可
    int local_conn = socket(PF_INET, SOCK_STREAM, 0);
    if(local_conn<0){
        LOG(ERROR) << "create local_conn socket failed";
        return -1;
    }
    struct sockaddr_in& local_addr = locals[remote_port];
    int ret = connect(local_conn, (struct sockaddr*)&local_addr, sizeof(local_addr));
    if(ret!=0){
        LOG(ERROR) << "connect local_port " << ntohs(local_addr.sin_port) << " failed!";
        close_file(local_conn);
        return -1;
    }
    return local_conn;
    // 后续准备向远程发送ok状态码(就是short 0)
}

// 进行状态发送
int FRPCManager::send_state(){
    int length=sizeof(short), buffer_idx=0;
    int bytes_write = 0;
    char* buffer = new char[length];
    *(short*)buffer = 0;
    while(true){
        if(buffer_idx>=length){
            delete [] buffer;
            return 0;
        }
        bytes_write = send(fd1, buffer+buffer_idx, length-buffer_idx, MSG_NOSIGNAL);
        if(bytes_write==-1){
            if(errno==EAGAIN || errno == EWOULDBLOCK){
                LOG(ERROR) << "the kernel buffer is not enough to send state";
                return -1;
            }
            LOG(ERROR) << "the frps error";
            return -1;
        }
        else if(bytes_write==0){
            LOG(ERROR) << "the frps closed";
            return -1;
        }
        buffer_idx += bytes_write;
    }
    delete [] buffer;
    return 0;    
}

RET_CODE FRPCManager::read_fd1(){
    int bytes_read = 0;
    while(true){
        if(forward_read_idx>=BIG_BUFFER_SIZE){
            return BUFFER_FULL;
        }
        bytes_read = recv(fd1, forward_buffer+forward_read_idx, BIG_BUFFER_SIZE-forward_read_idx, 0);
        if(bytes_read==-1){
            // 内核没数据可读
            if(errno==EAGAIN || errno==EWOULDBLOCK) break;
            return IOERR;
        }
        else if(bytes_read==0) return CLOSED;
        forward_read_idx+=bytes_read;
        LOG(INFO) << "bytes read: " << bytes_read << " ";
    }
    return (forward_read_idx-forward_write_idx>0)? OK: NOTHING;
}

RET_CODE FRPCManager::read_fd2(){
    int bytes_read = 0;
    while(true){
        if(backward_read_idx>=BIG_BUFFER_SIZE){
            return BUFFER_FULL;
        }
        bytes_read = recv(fd2, backward_buffer+backward_read_idx, BIG_BUFFER_SIZE-backward_read_idx, 0);
        if(bytes_read==-1){
            // 内核没数据可读
            if(errno==EAGAIN || errno==EWOULDBLOCK) break;
            return IOERR;
        }
        else if(bytes_read==0) return CLOSED;
        backward_read_idx+=bytes_read;
        LOG(INFO) << "bytes read: " << bytes_read << " ";

    }
    return (backward_read_idx-backward_write_idx>0)? OK: NOTHING;
}

RET_CODE FRPCManager::write_fd1(){
    int bytes_write = 0;
    while(true){
        // 正常退出都是buffer_empty
        if(backward_write_idx>=backward_read_idx){
            backward_write_idx = backward_read_idx=0;
            return BUFFER_EMPTY;
        }
        bytes_write = send(fd1, backward_buffer+backward_write_idx, backward_read_idx-backward_write_idx, MSG_NOSIGNAL);
        if(bytes_write==-1){
            // 内核没地方可写
            if(errno==EAGAIN || errno==EWOULDBLOCK) return TRY_AGAIN;
            return IOERR;
        }
        else if(bytes_write==0) return CLOSED;
        backward_write_idx+=bytes_write;
        LOG(INFO) << "bytes write: " << bytes_write << " ";

    }
    return OK;
}

RET_CODE FRPCManager::write_fd2(){
    int bytes_write = 0;
    while(true){
        if(forward_write_idx>=forward_read_idx){
            forward_write_idx=forward_read_idx=0;
            return BUFFER_EMPTY;
        }
        bytes_write = send(fd2, forward_buffer+forward_write_idx, forward_read_idx-forward_write_idx, MSG_NOSIGNAL);
        if(bytes_write==-1){
            if(errno==EAGAIN || errno==EWOULDBLOCK) return TRY_AGAIN;
            return IOERR;
        }
        else if(bytes_write==0) return CLOSED;
        forward_write_idx+=bytes_write;
        LOG(INFO) << "bytes write: " << bytes_write << " ";

    }
    return OK;
}