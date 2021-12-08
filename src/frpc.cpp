#include <string>
#include <fstream>
#include <pthread.h>
#include "cmdline.hpp"
#include "frpcmaster.hpp"
#include "util.hpp"

using namespace std;

#include "json.hpp"
using nlohmann::json;

json parse_json(const string& cfg_file){
    ifstream file(cfg_file);
    if(!file){
        cout << "the config path: " << cfg_file << " does not exists" << endl;
        exit(1);
    }
    json configs;
    file >> configs;
    return configs;
}

int main(int argc, char* argv[]){
    string cfg_file = parse_args(argc, argv);
    cout << "the config file path is: " << cfg_file << endl;
    json configs = parse_json(cfg_file);
    string serv_addr = configs["common"]["server_addr"];
    cout << serv_addr << endl;
    short server_port = configs["common"]["server_port"];
    cout << server_port << endl;
    short local_port = configs["service"][0]["local_port"];
    cout << local_port << endl;
    short remote_port = configs["service"][0]["remote_port"];
    cout << remote_port << endl;

    Master master(serv_addr, server_port, local_port, remote_port);
    master.start();
    pthread_exit(NULL);
}