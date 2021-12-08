#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "manager.hpp"
#include "util.hpp"

#include <iostream>
using namespace std;

// 利用manager来进行通信管理，需要交付manger通信两端的connection
// 本地只需要侦听listenfd，通信两端的连接交付给一个manager去侦听

int main(){
    const int EVENTS_SIZE = 5;
    int epollfd = epoll_create(1);
    epoll_event events[EVENTS_SIZE];
    int port=6996;
    
    struct sockaddr_in local_addr, addr, client_addr; // 本地22端口的地址 侦听地址 客户端连接地址
    int length=sizeof(addr), ret, num;
    socklen_t sock_len = sizeof(addr);

    // 建立和本地22端口的连接
    bzero(&local_addr, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    // 这个输入是数值类型而非字符串
    // local_addr.sin_addr.s_addr = htonl("127.0.0.1");
    inet_pton( AF_INET, "192.168.66.18", &local_addr.sin_addr );
    local_addr.sin_port = htons(22);

    int sockfd = socket(PF_INET, SOCK_STREAM, 0);
    if(sockfd<0){
        perror("create socket failed!");
        exit(1);
    }
    ret = connect(sockfd, (struct sockaddr*)&local_addr, sizeof(local_addr));
    if(ret!=0){
        perror("connect failed!");
        close(sockfd);
        exit(1);
    }

    // 本地启动侦听
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
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void*)&opt, sizeof(opt));

    ret=bind(listenfd, (struct sockaddr*)&addr, sizeof(addr));
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

    bool stop=false;
    Manager* manager;
    int client_conn=-1, conn;
    while(!stop){
        cout << "the frpc listening at port: " << port << endl;
        num = epoll_wait(epollfd, events, EVENTS_SIZE, -1);
        if(num==-1){
            perror("epoll_wait failed!");
            exit(1);
        }
        for(int i=0;i<num;++i){
            if(events[i].data.fd==listenfd && events[i].events | EPOLLIN){
                conn = accept(listenfd, (struct sockaddr*)&client_addr, &sock_len);
                if(conn==-1){
                    perror("accept failed!");
                    exit(1);
                }
                if(client_conn!=-1){
                    cout << "only allow one client!" << endl;
                }else{
                    client_conn = conn;
                    manager=new Manager(client_conn, sockfd);
                    Manager::start_routine(manager);
                    stop=true;
                }
            }
            // 其他情况则出错
            else{
                cout << "this situation should not be appeared!" << endl;
                stop = true;
            }
        }
    }

    close(listenfd);    
    return 0;
}