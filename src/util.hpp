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

void add_readfd(int epollfd, int fd){
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    cout << "add readfd" << endl;
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

void close_fd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
    fd=NULL;
}