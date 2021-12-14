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

#include "util.hpp"
#include "easylogging++.h"

class FRPSManager{
private:
    static const int BIG_BUFFER_SIZE;
    static const int EVENTS_SIZE;

    char *forward_buffer, *backward_buffer;
    int forward_read_idx, forward_write_idx;
    int backward_read_idx, backward_write_idx;

    // 需要进行发送的端口号
    short port;

// 对象方法
public:
    FRPSManager(short port, int ffd1, int ffd2);
    ~FRPSManager();
    // 侦听
    int epollfd;
    int fd1, fd2;
    int send_port();
    int recv_state();

    // 底层socket读写
    RET_CODE read_fd1();
    RET_CODE write_fd1();
    RET_CODE read_fd2();
    RET_CODE write_fd2();

// 类的方法
public:
    static void* start_frps_routine(void* arg);
};

// 定义
const int FRPSManager::EVENTS_SIZE = 5;
const int FRPSManager::BIG_BUFFER_SIZE = 65535;

FRPSManager::FRPSManager(short p, int ffd1, int ffd2):
        port(p), fd1(ffd1), fd2(ffd2),
        forward_read_idx(0), forward_write_idx(0),
        backward_read_idx(0), backward_write_idx(0){
    
    forward_buffer = new char[BIG_BUFFER_SIZE];
    backward_buffer = new char[BIG_BUFFER_SIZE];
    epollfd = epoll_create(1);
}

FRPSManager::~FRPSManager(){
    delete []forward_buffer;
    delete []backward_buffer;
    close_file(fd1);
    close_file(fd2);
    close_file(epollfd);
}

void* FRPSManager::start_frps_routine(void* arg){
    FRPSManager* manager = (FRPSManager*)arg;
    int epollfd = manager->epollfd;
    // fd1是client，fd2是frpc
    int fd1 = manager->fd1, fd2 = manager->fd2;
    // 准备发送端口号
    add_writefd(epollfd, fd2);
    epoll_event events[EVENTS_SIZE];

    int ret, num;
    RET_CODE res;
    bool stop=false, comm_out=true, comm_in=true;
    while(!stop){
        int num = epoll_wait(epollfd, events, EVENTS_SIZE, -1);
        if(num==-1){
            LOG(ERROR) << "epoll_wait failed!";
            break;
        }
        for(int i=0;i<num;++i){
            // fd2可写 需要先来进行判断是否是通信的开始阶段
            if(events[i].data.fd == fd2 && (events[i].events & EPOLLOUT)){
                if(comm_out){
                    ret = manager->send_port();
                    if(ret==-1){
                        stop = true;
                        break;
                    }
                    comm_out = false;
                    // 侦听来自frpc的ok状态码
                    modfd(epollfd, fd2, EPOLLIN);
                }
                else{
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
                
            }
            // 读取fd2数据 区分是通信的开始还是正常的数据交互
            else if(events[i].data.fd == fd2 && (events[i].events & EPOLLIN)){
                if(comm_in){
                    ret = manager->recv_state();
                    if(ret==-1){
                        stop = true;
                        break;
                    }
                    // 通信的准备工作完善 开始进行数据交互逻辑
                    add_readfd(epollfd, fd1);
                    modfd(epollfd, fd2, EPOLLIN);
                    comm_in = false;
                }
                else{
                    ret = manager->read_fd2();
                    LOG(INFO) << "read from " << fd2;
                    switch(ret){
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
            }
            //读取fd1数据
            if(events[i].data.fd == fd1 && (events[i].events & EPOLLIN)){
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
                        //关闭连接交给了析构函数来执行
                        stop=true;
                        break;
                    }
                    default:
                        break;
                }
            }
            // 发送给fd1端
            else if(events[i].data.fd == fd1 && (events[i].events & EPOLLOUT)){
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
            // 其他事件数据错误
            else{
                perror("the event is is not right");
                stop=true;
            }
        }
    }
    delete manager;
    return nullptr;
}

int FRPSManager::send_port(){
    int buffer_idx = 0, length=sizeof(port);
    int bytes_write = 0;
    char buffer[length];
    *((short*)buffer) = port;
    while(true){
        // 发送完毕
        if(buffer_idx>=length){
            return 0;
        }
        bytes_write = send(fd2, buffer+buffer_idx, length-buffer_idx, MSG_NOSIGNAL);
        if(bytes_write==-1){
            if(errno==EAGAIN || errno==EWOULDBLOCK){
                LOG(ERROR) << "the kernel buffer is not enough to send port";
                return -1;
            }
            LOG(ERROR) << "the frpc error";
            return -1;
        }
        else if(bytes_write==0){
            LOG(ERROR) << "the frpc closed";
            return -1;
        }
        buffer_idx += bytes_write;
    }
    return 0;
}

int FRPSManager::recv_state(){
    int buffer_idx = 0, length=sizeof(short);
    int bytes_read = 0;
    char buffer[length];
    while(true){
        if(buffer_idx>=length){
            LOG(ERROR) << "the buffer is not enough";
            return  -1;
        }
        bytes_read = recv(fd2, buffer+buffer_idx, length-buffer_idx, 0);
        if(bytes_read==-1){
            if(errno==EAGAIN || errno==EWOULDBLOCK) break;
            LOG(ERROR) << "the frpc connection is error";
            return -1;
        }
        else if(bytes_read==0){
            LOG(ERROR) << "the frpc closed";
            return -1;
        }
        buffer_idx += bytes_read;
    }
    if(buffer_idx==0){
        LOG(ERROR) << "frpc send nothing";
        return -1;
    }
    if(*(short*)buffer==0) return 0;
    return -1;
}

RET_CODE FRPSManager::read_fd1(){
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

RET_CODE FRPSManager::read_fd2(){
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

RET_CODE FRPSManager::write_fd1(){
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

RET_CODE FRPSManager::write_fd2(){
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