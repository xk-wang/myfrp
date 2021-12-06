#include <pthread.h>
#include <string>
#include "cmdline.hpp"
#include "frpsmaster.hpp"

using namespace std;

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
    Master master(bind_port);
    master->start();    
    pthread_exit(NULL);
}