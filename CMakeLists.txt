cmake_minimum_required(VERSION 3.2)
project(frpc)

set(CMAKE_CXX_STANDARD 11)

include_directories(include)
include_directories(src)

add_executable(${PROJECT_NAME} src/frpctest.cpp)