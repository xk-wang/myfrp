#include <string>
#include <fstream>
#include <pthread.h>
#include <unordered_map>
#include "cmdline.hpp"
#include "frpcmaster.hpp"
#include "util.hpp"
#include "easylogging++.h"
#include "json.hpp"
using namespace std;
using nlohmann::json;
INITIALIZE_EASYLOGGINGPP

json parse_json(const string& cfg_file){
    ifstream file(cfg_file);
    if(!file){
        LOG(ERROR) << "the config path: " << cfg_file << " does not exists";
        exit(1);
    }
    json configs;
    file >> configs;
    return configs;
}

int main(int argc, char* argv[]){
    string cfg_file = parse_args(argc, argv);
    json configs = parse_json(cfg_file);
    string serv_addr = configs["common"]["server_addr"];
    short server_port = configs["common"]["server_port"];
    // 端口读取需要构建一个remote_port-->local_port的映射
    std::unordered_map<short, short>ports;
    for(int i=0;i<configs["service"].size();++i){
        ports[configs["service"][i]["remote_port"]] = configs["service"][i]["local_port"];
    }
    Master master(serv_addr, server_port, ports);
    master.start();
    pthread_exit(NULL);
    return 0;
}