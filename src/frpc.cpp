#include <string>
#include <fstream>
#include <pthread.h>
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
    short local_port = configs["service"][0]["local_port"];
    short remote_port = configs["service"][0]["remote_port"];

    Master master(serv_addr, server_port, local_port, remote_port);
    master.start();
    pthread_exit(NULL);
    return 0;
}