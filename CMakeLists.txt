# for frpctest

# cmake_minimum_required(VERSION 3.2)
# project(frpctest)

# set(CMAKE_CXX_STANDARD 11)

# include_directories(include)
# include_directories(src)

# add_executable(${PROJECT_NAME} src/frpctest.cpp)


cmake_minimum_required(VERSION 3.2)
project(frp)
set(CMAKE_CXX_STANDARD 11)
include_directories(include)
include_directories(src)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
# for frps
add_executable(frps src/frps.cpp)
target_link_libraries(frps Threads::Threads)

# for frpc
add_executable(frpc src/frpc.cpp)
target_link_libraries(frpc Threads::Threads)