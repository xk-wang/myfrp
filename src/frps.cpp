#include <string>
#include <fstream>
#include <pthread.h>
#include "cmdline.hpp"
#include "frpsmaster.hpp"
#include "util.hpp"
#include "easylogging++.h"
#include "json.hpp"
using namespace std;
using nlohmann::json;

INITIALIZE_EASYLOGGINGPP

int parse_json(const string& cfg_file){
    ifstream file(cfg_file);
    if(!file){
        LOG(ERROR) << "the config path: " << cfg_file << " does not exists";
        exit(1);
    }
    json configs;
    file >> configs;
    int port = configs["bind_port"];
    return port;
}

int main(int argc, char* argv[]){
    string cfg_file = parse_args(argc, argv);
    int bind_port = parse_json(cfg_file);
    Master master(bind_port);
    master.start();    
    pthread_exit(NULL);
}