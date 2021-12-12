# for frpctest

# cmake_minimum_required(VERSION 3.20)
# project(frpctest)

# set(CMAKE_CXX_STANDARD 11)

# include_directories(include)
# include_directories(src)

# add_executable(${PROJECT_NAME} src/frpctest.cpp)


cmake_minimum_required(VERSION 3.20)
project(frp)
set(CMAKE_CXX_STANDARD 11)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# for frps
add_executable(frps src/easylogging++.cpp src/frps.cpp)
target_include_directories(frps PRIVATE include)
target_include_directories(frps PRIVATE src)
target_link_libraries(frps PRIVATE Threads::Threads)

# for frpc
add_executable(frpc src/easylogging++.cpp src/frpc.cpp)
target_include_directories(frpc PRIVATE include)
target_include_directories(frpc PRIVATE src)
target_link_libraries(frpc PRIVATE Threads::Threads)