cmake_minimum_required(VERSION 3.16)
project(p2p_file_sharing CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Emit compile_commands.json so editors/clangd resolve the src/ include path.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_library(p2p_common STATIC
  src/common/socket_io.cpp
  src/common/message.cpp
  src/common/hash.cpp
  src/common/auth.cpp
)
target_include_directories(p2p_common PUBLIC src)
target_link_libraries(p2p_common PUBLIC Threads::Threads OpenSSL::Crypto)

add_executable(tracker
  src/tracker/main.cpp
  src/tracker/tracker_state.cpp
  src/tracker/session_manager.cpp
  src/tracker/persistence.cpp
)
target_link_libraries(tracker PRIVATE p2p_common)

add_executable(client src/client/main.cpp)
target_link_libraries(client PRIVATE p2p_common)
