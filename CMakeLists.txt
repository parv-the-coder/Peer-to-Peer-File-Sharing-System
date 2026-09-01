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
  src/common/config.cpp
)
target_include_directories(p2p_common PUBLIC src)
target_link_libraries(p2p_common PUBLIC Threads::Threads OpenSSL::Crypto)

add_executable(tracker
  src/tracker/main.cpp
  src/tracker/tracker_state.cpp
  src/tracker/session_manager.cpp
  src/tracker/persistence.cpp
  src/tracker/peer_link.cpp
)
target_link_libraries(tracker PRIVATE p2p_common)

add_executable(client
  src/client/main.cpp
  src/client/upload_registry.cpp
  src/client/peer_server.cpp
  src/client/tracker_client.cpp
  src/client/downloader.cpp
  src/client/commands.cpp
)
target_link_libraries(client PRIVATE p2p_common)

option(P2P_BUILD_TESTS "Build unit tests" ON)
if(P2P_BUILD_TESTS)
  enable_testing()

  add_executable(unit_tests
    tests/unit/main.cpp
    tests/unit/test_hash.cpp
    tests/unit/test_message.cpp
    tests/unit/test_auth.cpp
    tests/unit/test_session.cpp
    tests/unit/test_tracker_state.cpp
    src/tracker/tracker_state.cpp
    src/tracker/session_manager.cpp
  )
  target_include_directories(unit_tests PRIVATE tests/unit)
  target_link_libraries(unit_tests PRIVATE p2p_common)
  add_test(NAME unit COMMAND unit_tests)

  # Integration tests drive the real binaries, so they need them built.
  foreach(t e2e disconnect persistence concurrent_download tracker_sync)
    add_test(NAME ${t}
             COMMAND ${CMAKE_COMMAND} -E env bash
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/${t}_test.sh)
    set_tests_properties(${t} PROPERTIES TIMEOUT 300)
  endforeach()
endif()
