#pragma once

#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include <string>
#include <fstream>
#include <vector>
#include <unordered_map>
using namespace std;

#include "json.hpp"
using namespace nlohmann::json;

int parse_json(string& cfg_file){
    ifstream file(cfg_file);
    json configs;
    file >> configs;
    int port = configs["bind_port"];
    return port;
}

json deserialization_json(char* origin_str){
    string str=origin_str;
    return json::parse(str);
}

char* serialization_json(json& js){
    string str=js.dump()
    return str.c_str();
}

int set_nonblocking(int fd){
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    assert(fcntl(fd, F_SETFL, new_option)>=0);
    return old_option;
}

void add_readfd(int epollfd, int fd){
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    set_nonblocking(fd);
}

void add_writefd(int epollfd, int fd){
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLOUT | EPOLLET;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    set_nonblocking(fd);
}

void close_fd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

int read_buffer(int fd, char* buffer, int length){
    memset(buffer, '\0', length);
    int bytes_read = recv(fd, buffer, length, 0);
    if(bytes_read==-1 && errno!=AGAIN && errno!=EWOULDBLOCK){
        perror("read fd error");
        return -2; // -2代表IOERROR
    }
    return bytes_read;
}

int write_buffer(int fd, char* buffer, int length){
    // 防止sigpipe信号中断进程
    int bytes_write = send(fd, buffer, length, MSG_CONFIRM);
    memset(buffer, '\0', length);
    if(bytes_write==-1 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EPIPE){
        perror("write fd error");
        return -2;
    }
    if(errno==EAGAIN||errno==EWOULDBLOCK) return -1; // 不能发送了
    if(errno==EPIPE) return 0; // 关闭连接 
    return bytes_write; // 发送字节数
}


// 确定frpc和frps之间的通信格式如下：
// 1.第一个字节：0表示frpc发送给frps的配置信息
// 1表示frps向frpc请求建立新连接给新的服务使用
// code字段；
// 2.第二个字节代表配置服务的数量，这样最多支持256个服务 service_len；
// 3.从第三个字节开始，每半字读取一下数值，分别代表remote_port和local_port;
// 4.每个服务包含1一个字的长度，因此每个n个服务需要n个int长度

unordered_map<short, short> parse_config(const char* const origin_buffer, int length){
    char* buffer=origin_buffer;
    char code = *buffer++;
    if(code!=0){
        cout << "the code number from frpc is not right!" << endl;
        exit(1);
    }
    char service_len = *buffer++;
    if(service_len<=0){
        cout << "the service length is negative!" <<endl;
        exit(1);
    }
    int max_service_length = (length-2)/4;
    if(max_service_length<service_length){
        cout << "the service length is greater than the buffer capacity!" << endl;
        exit(1);
    }
    // remote_port --> local_port
    unordered_map<short, short>ports;
    short local_port, remote_port;
    for(int i=0, short* p=(short*)buffer;i<service_len;++i){
        local_port = *p++;
        remote_port = *p++;
        ports[remote_port] = local_port;
    }
    return ports;
}

struct PORT{
public:
    short remote_port;
    short local_port;
    int father_listenfd;
    unordered_map<short, bool>&communications;
    PORT(short port1, int port2, int listenfd, unordered_map<short, bool>&comm): communications(comm){
        remote_port = port1;
        local_port = port2;
        father_listenfd = listenfd;
    }
}

// 主线程启动相应的子线程启动服务并且开始侦听
// 主线程不在负责侦听这些服务的端口
void start_services(const unordered_map<short, short>&ports, unordered_map<short, vector<bool>>&communications,
                    int listenfd){
    //懒汉模式，等到client请求连接再去请求frpc的主动简历连接
    //运行起来后，每个线程都是死循环，脱离主线程
    //创建分离式线程
    pthread_attr_t attr;
    int ret=pthread_attr_init(&attr);
    if(ret!=0){
        perror("init pthread attr failed!");
        exit(1);
    }
    ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if(ret!=0){
        perror("pthread_attr_setdetachstate failed!");
        exit(1);
    }
    short remote_port, local_port;
    for(auto iter=ports.begin();iter!=ports.end();++port){
        communications[remote_port] = vector<bool>{false, false};
        struct PORT* port = new PORT(iter->first, iter->second, listenfd, communications);
        ret = pthread_create(&tid, &attr, start_routine, port);
        if(ret!=0){
            perror("pthread create failed!");
            exit(1);
        }
    }
    ret = pthread_attr_destroy(&attr);
    if(ret!=0){
        perror("pthread_attr_destroy failed!");
        exit(1);
    }
}

void* start_child_child_routine(void* origin_arg){
    struct CHILD_CHILD_ARG* arg = (struct CHILD_CHILD_ARG*)origin_arg;
    int ff_listenfd = arg->father_father_listenfd;
    int f_listenfd = arg->father_listenfd;

    struct sockaddr_in frpc_addr, client_addr;
    int length = sizeof(frpc_addr);
    
    int frpc_fd = accept(ff_listenfd, (sockaddr*)&frpc_addr, &length);
    if(frpc_fd==-1){
        perror("accept frpc failed!");
        exit(1);
    }

    int client_fd = accpet(f_listenfd, (sockaddr*)&client_addr, &length);
    if(client_fd==-1){
        peror("accept cient failed!");
        exit(1);
    }

    int epollfd = epoll_clt(1);
    add_readfd(epollfd, frpc_fd);
    add_readfd(epollfd, client_fd);
    epoll_event events[EVENTS_SIZE];

    char* client2frpc_buffer = new char[BIG_BUFFER_SIZE];
    char* frpc2client_buffer = new char[BIG_BUFFER_SIZE];
    // 每个缓冲区需要两个idx来指定读取的开始位置和发送的开始位置
    int client2frpc_read_idx=0, client2frpc_write_idx=0;
    int frpc2client_read_idx=0, frpc2client_write_idx=0;

    bool stop=false;

    while(!stop){
        int num = epoll_wait(epollfd, events, EVENTS_SIZE, -1);
        if(num==-1){
            perror("epoll_wait failed!");
            exit(1);
        }
        int ret;
        for(int i=0;i<num;++i){
            // 从客户端读取数据
            if(events[i].data.fd == client_fd && events[i].events | EPOLLIN){
                if(client2frpc_write_idx>=client2frpc_read_idx){
                    client2frpc_write_idx=client2frpc_read_idx=0;
                }
                while(client2frpc_read_idx<BIG_BUFFER_SIZE){
                    ret=read_buffer(client_fd, client2frpc_buffer+client2frpc_read_idx, BIG_BUFFER_SIZE-client2frpc_read_idx);
                    if(ret==-1){ // 读取完毕，需要退出
                        break;    
                    }else if(ret==0||ret==-2){ //客户端关闭连接，关闭socket连接退出
                        close_fd(epollfd, client_fd);
                        close_fd(epollfd, frpc_fd);
                        stop=true;
                        break;
                    }else if(ret>0){
                        client2frpc_read_idx+=ret;
                    }
                }
                // 准备写数据到frpc，之后写数据从0开始写到client2frpc_idx
                add_writefd(epollfd, frpc_fd);
            }
            // 从frpc读数据
            else if(events[i].data.fd == frpc_fd && events[i].events | EPOLLIN){
                if(frpc2client_write_idx>=frpc2client_read_idx){
                    frpc2client_write_idx=frpc2client_read_idx=0;
                }
                // 必须要全部读完，否则后续不会通知数据
                while(frpc2client_read_idx<BIG_BUFFER_SIZE){
                    ret=read_buffer(frpc_fd, frpc2client_buffer+frpc2client_read_idx, BIG_BUFFER_SIZE-frpc2client_read_idx);
                    if(ret==-1){ // 读取完毕 需要退出
                        break;
                    }else if(ret==0||ret==-2){
                        close_fd(epollfd, frpc_fd);
                        close_fd(epollfd, client_fd);
                        stop=true;
                        break;
                    }else if(ret>0){
                        frpc2client_read_idx+=ret;
                    }
                    // 准备写数据到client
                    add_writefd(epollfd, client_fd);
                }
            }
            // 发送给client
            else if(events[i].data.fd == client_fd && events[i].events | EPOLLOUT){
                while(frpc2client_write_idx<frpc2client_read_idx){
                    ret=write_buffer(client_fd, frpc2client_buffer+frpc2client_write_idx, frpc2client_read_idx-frpc2client_write_idx);
                    if(ret==-1){ // 不能发送了（内核缓冲区不够用）
                        break;
                    }else if(ret==0||ret==-2){
                        close_fd(epollfd, client_fd);
                        close_fd(epollfd, frpc_fd);
                        stop=true;
                        break;
                    }else if(ret>0){
                        frpc2client_write_idx+=ret;
                    }
                }
                // 准备读数据
                add_readfd(epollfd, frpc_fd);             
            }
            // 发送给frpc
            else if(events[i].data.fd == frpc_fd && events[i].events | EPOLLOUT){
                while(client2frpc_write_idx<client2frpc_read_idx){
                    ret=write_buffer(frpc_fd, client2frpc_buffer+client2frpc_write_idx, client2frpc_read_idx-client2frpc_write_idx);
                    if(ret==-1){ // 不能发送 
                        break;
                    }else if(ret==0||ret==-2){ // 关闭连接
                        close_fd(epollfd, frpc_fd);
                        close_fd(epollfd, client_fd);
                        stop=true;
                        break;
                    }else if(ret>0){
                        client2frpc_write_idx+=ret;
                    }
                }
                add_readfd(epollfd, client_fd);
            }
        }
    }
    delete [] client2frpc_buffer;
    delete [] frpc2client_buffer;
    return nullptr;
}

struct CHILD_CHILD_ARG{
public:
    int father_father_listenfd;
    int father_listenfd;
    CHILD_CHILD_ARG(int listenfd1, int listenfd2){
        father_father_listenfd = listenfd1;
        father_listenfd = listenfd2;
    }
}


void* start_routine(void* arg){
    struct PORT* port = (struct PORT*)arg;
    auto comm = port->communications;

    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port->remote_port);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd>=0);
    int opt=1;
    setsockopt(listenfd, SOL_SOCKET, (const void*)*opt, sizeof(opt));

    int ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret!=-1);
    ret = listen(listenfd, 5);
    assert(ret!=-1);

    int epollfd = epoll_clt(1);
    add_readfd(epollfd, listenfd);
    epoll_event events[EVENTS_SIZE];

    pthread_attr_t attr;
    int ret=pthread_attr_init(&attr);
    if(ret!=0){
        perror("init pthread attr failed!");
        exit(1);
    }
    ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if(ret!=0){
        perror("pthread_attr_setdetachstate failed!");
        exit(1);
    }

    while(true){
        int num = eoll_wait(epollfd, events, EVENTS_SIZE, -1);
        if(num==-1){
            cout << "epoll_wait failed!" << endl;
            exit(1);
        }
        // 父线程和子线程之间通过全局变量，子线程向父进程发送让frpc建立连接的请求
        // 通过端口号识别，每个线程维护两个缓冲区，用来标识父子线程之间的通信状态
        // 子线程从listenfd中accept来自frpc的连接
        
        for(int i=0;i<num;++i){
            // 来了新连接，子线程直接让frpc发送请求连接，这个连接由父线程交付给相应的子线程，
            if(events[i].data.fd==listenfd){
                comm[port->remote_port][0]=true;
                for(;!comm[port->remote_port][1];sleep(1));
                comm[port->remote_port][1]=false;
                
                // 生成子线程来接收连接
                // 这里添加锁，父线程先生成一定量的连接备用，之后子线程来竞争
                // 每次父进程处理完侦听事件后，进行判断剩余的连接数量，如果不够用
                // 向frpc发送新的建立连接请求，每次父进程发送一个数字代表需要建立连接的数量
                struct CHILD_CHILD_ARG* arg = new struct CHILD_CHILD_ARG(port->father_listenfd, listenfd); 
                ret = pthread_create(&tid, &attr, start_child_child_routine, arg);
                if(ret!=0){
                    perror("pthread create failed!");
                    exit(1);
                }
                sleep(1); //保证子子线程能够accept连接之后再去处理下一个连接请求
            }
        }
    }
    ret = pthread_attr_destroy(&attr);
    if(ret!=0){
        perror("pthread_attr_destroy failed!");
        exit(1);
    }
    return nullptr;
}