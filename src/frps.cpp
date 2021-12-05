#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>

#include <string>
#include <unordered_map>
#include <iostream>
using namespace std;
#include "cmdline.hpp"
#include "util.hpp"

#define EVENTS_SIZE 5
#define BUFFER_SIZE 4096  //保证能够支持256服务的需要

char* buffer = new char[BUFFER_SIZE];

string parse_args(int argc, char* argv[]) {
    cmdline::parser parser;
    parser.add<string>("cfg_file", 'f', "the config file path", false, "");
    parser.parse_check(argc, argv);
    string cfg_file = parser.get<string>("cfg_file");
    return cfg_file;
}

int main(int argc, char* argv[]){
    string cfg_file = parse_args(argc, argv);
    int bind_port = parse_json(cfg_file);

    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(bind_port);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd>=0);
    int opt=1;
    setsockopt(listenfd, SOL_SOCKET, (const void*)&opt,sizeof(opt));

    int ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret!=-1);
    ret = listen(listenfd, 5);
    assert(ret!=-1);

    int epollfd = epoll_clt(1);
    add_readfd(epollfd, listenfd);
    epoll_event events[EVENTS_SIZE]; //只保持和frpc的通信，不需要去侦听很多内容，每个服务的侦听交给子线程去执行

    struct sockaddr_in frpc_addr;
    int length = sizeof(frpc_addr);
    unordered_map<short, short>ports;
    // 第一位表示子线程需要父线程处理，第二位表示父线程需要子线程处理
    unordered_map<short, vector<bool>>communications;
    int frpc_fd;

    int bytes_read;

    while(true){
        int num= epoll_wait(epollfd, events, EVENTS_SIZE, -1);
        if(num==-1){
            cout << "frps epoll_wait failed!" <<endl;
            exit(1);
        }
        //后续存在两个侦听描述符
        //启动多个服务，启动多个frpc, 多个用户来测试
        for(int i=0;i<num;++i){
            if(events[i].data.fd==listenfd){
                //先建立和frpc的连接之后通过这个唯一的连接来和frpc进行通信
                frpc_fd = accept(listenfd, (sockaddr*)&frpc_addr, &length);
                if(frpc_fd==-1){
                    cout << "frps accept frpc failed!" << endl;
                    exit(1);
                }
                add_readfd(epollfd, fd);
            }else if((events[i].data.fd==frpc_fd) && (events[i].events|EPOLLIN){
                //非阻塞需要来循环读
                while((bytes_read = read_buffer(frpc_fd, buffer, BUFFER_SIZE))>-1){
                    if(bytes_read==-1){
                        cout << "read date complete!" << endl;
                        break;
                    }
                    else if(bytes_read==0){
                        cout << "one frpc client closed connection!" << endl;
                        close_fd(epollfd, frpc_fd);
                        break;;
                    }else{
                        ports = parse_config(buffer, BUFFER_SIZE);
                        start_services(ports, listenfd, communications);
                    }
                }
            }else if((events[i].data.fd==frpc_fd) && (events[i].envents|EPOLLOUT){

            }
        }
    }

    return 0;
}