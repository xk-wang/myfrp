#pragma once

#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/inet.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "util.hpp"

// 此处的manager必须要设置joinable来保证由父进程回收缓冲区资源
// 声明
class Manager{
private:
    // 缓冲区
    static const int BIG_BUFFER_SIZE;
    static const int EVENTS_SIZE;
    int fd1, fd2;
    char *forward_buffer, *backward_buffer;
    int forward_read_idx, forward_write_idx;
    int backward_read_idx, backward_write_idx;
    // 侦听
    int epollfd;
    epoll_event* events;
    // 状态
    bool stop;
public:
    Manager(int ffd1, ffd2);
    ~Manager();
    static void* start_routine(void* arg);
}

// 定义
const int Manager::EVENTS_SIZE = 5;
const int Manager::BIG_BUFFER_SIZE = 65535;

Manager::Manager(int ffd1, ffd2): fd1(ffd1), fd2(ffd2),
        forward_read_idx(0), forward_write_idx(0),
        backward_read_idx(0), backward_write_idx(0),
        stop(false){
    forward_buffer = new char[BIG_BUFFER_SIZE];
    backward_buffer = new char[BIG_BUFFER_SIZE];
    epollfd = epoll_clt(1);
    events = new epoll_event[EVENTS_SIZE];
}

Manager::~Manager(){
    delete []forward_buffer;
    delete []backward_buffer;
    delete []events;
    close(fd1);
    close(fd2);
    close(epollfd);
}

void* Manager::start_routine(void* arg){
    Manager* manager = (Manager*)arg;
    int epollfd = manager->epollfd;
    int fd1 = manager->fd1, fd2 = manager->fd2;
    epoll_event* events = manager->events; 
    add_readfd(epollfd, fd1);
    add_readfd(epollfd, fd2);

    int ret, num;
    while(!stop){
        int ret = epoll_wait(epollfd, events, EVENTS_SIZE, -1);
        if(ret==-1){
            perror("epoll_wait failed!");
            exit(1);
        }
        for(int i=0;i<ret;++i){
            //从fd1端读取数据 缓冲区足够大，保证能通过循环读完
            if(events[i].data.fd == fd1 && events[i].events | EPOLLIN){
                if(forward_write_idx>=forward_read_idx) forward_write_idx=forward_read_idx=0;
                while(forward_read_idx<BIG_BUFFER_SIZE){
                    // 字节流不会被截断，边界会被忽略
                    num = read_buffer(fd1, forward_buffer+forward_read_idx, BIG_BUFFER_SIZE-forward_read_idx);
                    if(num==-1) break; // 读取完毕，需要退出
                    else if(num>0) forward_read_idx+=num; // 正常读取
                    else if(num==0||num==-2){ // fd1段关闭连接
                        close_fd(epollfd, fd1);
                        close_fd(epollfd, fd2);
                        stop=true;
                        break;
                    }
                }
                if(!stop) add_writefd(epollfd, fd2);
            }
            // 从fd2端读取数据
            else if(events[i].data.fd == fd2 && events[i].events | EPOLLIN){
                if(backward_write_idx>=backward_read_idx) backward_write_idx=backward_read_idx=0;
                while(backward_read_idx<BIG_BUFFER_SIZE){
                    num = read_buffer(fd2, backward_buffer+backward_read_idx, BIG_BUFFER_SIZE-backward_read_idx);
                    if(num==-1) break; // 读取完毕
                    else if(num>0) backward_read_idx += num;
                    else if(num==0||num==-2){
                        close_fd(epollfd, fd1);
                        close_fd(epollfd, fd2);
                        stop = true;
                        break;
                    }
                }
                if(!stop) add_writefd(epollfd, fd1);
            }
            // 发送给fd1端 内核缓冲区不够用，循环不一定能发完
            else if(events[i].data.fd == fd1 && events[i].events | EPOLLOUT){
                while(backward_write_idx<backward_read_idx){
                    num = write_buffer(fd1, backward_buffer+backward_write_idx, backward_read_idx-backward_write_idx);
                    if(num==-1) break; // 停止发送
                    else if(num>0) backward_write_idx += num;
                    else if(num==0||num==-2){
                        close_fd(epollfd, fd1);
                        close_fd(epollfd, fd2);
                        stop=true;
                        break;
                    }
                }
                if(!stop) add_readfd(epollfd, fd2);
            }
            // 发送给fd2端
            else if(events[i].data.fd == fd2 && events[i].events | EPOLLOUT){
                while(forward_write_idx<forward_read_idx){
                    num = write_buffer(fd2, forward_buffer+forward_write_idx, forward_read_idx-forward_write_idx);
                    if(num==-1) break;
                    else if(num>0) forward_write_idx  += num;
                    else if(num==0||num==-2){
                        close_fd(epollfd, fd1);
                        close_fd(epollfd, fd2);
                        stop=true;
                        break;
                    }
                }
                if(!stop) add_readfd(epollfd, fd1);
            }
        }
    }
    return nullptr;
}