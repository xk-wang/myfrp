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


class Manager{
private:
    // 缓冲区
    static const int BIG_BUFFER_SIZE;
    static const int EVENTS_SIZE;
    char *forward_buffer, *backward_buffer;
    int forward_read_idx, forward_write_idx;
    int backward_read_idx, backward_write_idx;
public:
    // 侦听
    int epollfd;
    int fd1, fd2;
public:
    Manager(int ffd1, int ffd2);
    ~Manager();
    static void* start_routine(void* arg);

    // socket读写
    RET_CODE read_fd1();
    RET_CODE write_fd1();
    RET_CODE read_fd2();
    RET_CODE write_fd2();
};

// 定义
const int Manager::EVENTS_SIZE = 5;
const int Manager::BIG_BUFFER_SIZE = 65535;

Manager::Manager(int ffd1, int ffd2): fd1(ffd1), fd2(ffd2),
        forward_read_idx(0), forward_write_idx(0),
        backward_read_idx(0), backward_write_idx(0){
    forward_buffer = new char[BIG_BUFFER_SIZE];
    backward_buffer = new char[BIG_BUFFER_SIZE];
    epollfd = epoll_create(1);
}

Manager::~Manager(){
    delete []forward_buffer;
    delete []backward_buffer;
    close(fd1);
    close(fd2);
    close(epollfd);
}

// 每个manager结束之后可以在内部进行manager的资源回收
void* Manager::start_routine(void* arg){
    Manager* manager = (Manager*)arg;
    int epollfd = manager->epollfd;
    int fd1 = manager->fd1, fd2 = manager->fd2;
    epoll_event events[EVENTS_SIZE]; 
    add_readfd(epollfd, fd1);
    add_readfd(epollfd, fd2);

    // add_readf和modfd都是会覆盖以前的操作，不同的是add_readfd和add_writefd是
    // 初次使用调用EPOLL_CTL_ADD，modfd是进行侦听文件描述符的修改EPOLL_CTL_MOD
    int ret, num;
    bool stop=false;
    while(!stop){
        int ret = epoll_wait(epollfd, events, EVENTS_SIZE, -1);
        if(ret==-1){
            perror("epoll_wait failed!");
            exit(1);
        }
        RET_CODE res;
        for(int i=0;i<ret;++i){
            //读取fd1数据
            if(events[i].data.fd == fd1 && (events[i].events & EPOLLIN)){
                res = manager->read_fd1();
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
            // 读取fd2数据
            else if(events[i].data.fd == fd2 && (events[i].events & EPOLLIN)){
                res = manager->read_fd2();
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
            // 发送给fd1端
            else if(events[i].data.fd == fd1 && (events[i].events & EPOLLOUT)){
                res = manager->write_fd1();
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
            // 发送给fd2端
            else if(events[i].data.fd == fd2 && (events[i].events & EPOLLOUT)){
                res = manager->write_fd2();
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
                perror("the event is is not right");
                stop=true;
            }
        }
    }
    delete manager;
    return nullptr;
}

RET_CODE Manager::read_fd1(){
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
    }
    return (forward_read_idx-forward_write_idx>0)? OK: NOTHING;
}

RET_CODE Manager::read_fd2(){
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
    }
    return (backward_read_idx-backward_write_idx>0)? OK: NOTHING;
}

RET_CODE Manager::write_fd1(){
    int bytes_write = 0;
    while(true){
        // 正常退出都是buffer_empty
        if(backward_write_idx>=backward_read_idx){
            backward_write_idx = backward_read_idx=0;
            return BUFFER_EMPTY;
        }
        bytes_write = send(fd1, backward_buffer+backward_write_idx, backward_read_idx-backward_write_idx, 0);
        if(bytes_write==-1){
            // 内核没地方可写
            if(errno==EAGAIN || errno==EWOULDBLOCK) return TRY_AGAIN;
            return IOERR;
        }
        else if(bytes_write==0) return CLOSED;
        backward_write_idx+=bytes_write;
    }
    return OK;
}

RET_CODE Manager::write_fd2(){
    int bytes_write = 0;
    while(true){
        if(forward_write_idx>=forward_read_idx){
            forward_write_idx=forward_read_idx=0;
            return BUFFER_EMPTY;
        }
        bytes_write = send(fd2, forward_buffer+forward_write_idx, forward_read_idx-forward_write_idx, 0);
        if(bytes_write==-1){
            if(errno==EAGAIN || errno==EWOULDBLOCK) return TRY_AGAIN;
            return IOERR;
        }
        else if(bytes_write==0) return CLOSED;
        forward_write_idx+=bytes_write;
    }
    return OK;
}