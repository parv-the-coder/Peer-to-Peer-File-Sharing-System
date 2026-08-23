cmake_minimum_required(VERSION 3.16)
project(p2p_file_sharing CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_library(p2p_common STATIC
  src/common/socket_io.cpp
  src/common/message.cpp
)
target_include_directories(p2p_common PUBLIC src)
target_link_libraries(p2p_common PUBLIC Threads::Threads)

add_executable(tracker src/tracker/main.cpp)
target_link_libraries(tracker PRIVATE p2p_common)

add_executable(client src/client/main.cpp)
target_link_libraries(client PRIVATE p2p_common OpenSSL::SSL OpenSSL::Crypto)
