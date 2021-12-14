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
#include <iostream>
#include <unordered_map>
using namespace std;

#include "json.hpp"
using nlohmann::json;

// 正常情况OK, 没数据处理NOTHING, IO错误IOERR, 对方关闭CLOSED, 缓冲区满BUFFER_FULL(读数据), 缓冲区空BUFFER_EMPTY(写数据), 再次尝试(内核缓冲区满，下次再写)
enum RET_CODE { OK = 0, NOTHING = 1, IOERR = -1, CLOSED = -2, BUFFER_FULL = -3, BUFFER_EMPTY = -4, TRY_AGAIN };
enum OP_TYPE { READ = 0, WRITE, ERROR }; // ERROR暂时没用到

string parse_args(int argc, char* argv[]) {
    cmdline::parser parser;
    parser.add<string>("cfg_file", 'c', "the config file path", false, "");
    parser.parse_check(argc, argv);
    string cfg_file = parser.get<string>("cfg_file");
    return cfg_file;
}

json deserialization_json(char* origin_str){
    string str=origin_str;
    return json::parse(str);
}

const char* serialization_json(json& js){
    string str=js.dump();
    return str.c_str();
}

int set_nonblocking(int fd){
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    assert(fcntl(fd, F_SETFL, new_option)>=0);
    return old_option;
}

// 这个是将之前的侦听进行了覆盖
// modfd基本上是一样的
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

void modfd(int epollfd, int fd, int ev){
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

void close_file(int fd){
    if(fd!=-1){
        close(fd);
        fd = -1;
    }
}

void close_fd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close_file(fd);
}