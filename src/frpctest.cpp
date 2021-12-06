#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/inet.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "util.hpp"

const int port = 2021;
const int EVENTS_SIZE = 5;
const int BUFFER_SIZE = 65535;

// 在本地启动一个server 建立和本地22端口的连接，充当本地转发器

int main(){
    int epollfd = epoll_clt(1);
    epoll_event events[EVENTS_SIZE];

    char* forward_buffer = new char[BUFFER_SIZE];
    char* backward_buffer = new char[BUFFER_SIZE];
    int foward_read_idx=0, foward_write_idx=0;
    int backward_read_idx=0, backward_write_idx=0;
    
    struct sockaddr_in local_addr, addr; // 本地22端口的地址和客户端连接地址
    int length=sizeof(addr), ret, num;

    // 建立和本地22端口的连接
    bzero(&local_addr, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl("127.0.0.1");
    local_addr.sin_port = htons(22);

    int sockfd = socket(PF_INET, SOCK_STREAM, 0);
    if(sockfd<0){
        perror("create socket failed!");
        exit(1);
    }
    ret = connect(sockfd, (struct sockaddr*)&address, sizeof(address));
    if(ret!=0){
        perror("connect failed!");
        close(sockfd);
        exit(1);
    }

    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
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

    int conn;
    bool stop=false;
    while(!stop){
        num = epoll_wait(epollfd, events, EVENTS_SIZE, -1);
        if(num==-1){
            perror("epoll_wait failed!");
            exit(1);
        }
        for(int i=0;i<num;++i){
            if(events[i].data.fd==listenfd && events[i].events | EPOLLIN){
                conn = accpet(listenfd, (struct sockaddr*)&addr, &length);
                if(conn==-1){
                    perror("accept failed!");
                    exit(1);
                }
                add_readfd(epollfd, conn);
            }
            // 读取客户端字节流
            else if(events[i].data.fd==conn && events[i].events | EPOLLIN){
                if(forward_write_idx>=forward_read_idx) forward_write_idx=forward_read_idx=0;
                while(forward_read_idx<BUFFER_SIZE){
                    ret = read_buffer(conn, forward_buffer+forward_read_idx, BUFFER_SIZE-forward_read_idx);
                    if(ret==-1) break; // 读取完毕，退出
                    else if(ret>0) forward_read_idx+=ret;
                    else if(ret==0||ret==-2){
                        close_fd(epollfd, conn);
                        close_fd(epollfd, listenfd);
                        stop=true;
                        break;
                    }
                }
                if(!stop) add_writefd(epollfd, sockfd);
            }
            // 发送给sshd端口
            else if(events[i].data.fd==sockfd && events[i].events | EPOLLOUT){
                while(forward_write_idx<forward_read_idx){
                    ret = write_buffer(sockfd, forward_buffer+forward_write_idx, forward_read_idx-forward_write_idx);
                    if(ret==-1) break; //停止发送
                    else if(num>0) forward_write_idx+=ret;
                    else if(num==0||num==-2){
                        close_fd(epollfd, conn);
                        close_fd(epollfd, listenfd);
                        stop=true;
                        break;
                    }
                }
                // 下一步需要从sockfd来读
                if(!stop) add_readfd(epollfd, sockfd);
            }
            // 读取sockfd的数据
            else if(events[i].data.fd==sockfd && events[i].events | EPOLLIN){
                if(backward_read_idx<)
            }
        }
    }
    
    return 0;
}