cmake_minimum_required(VERSION 3.5)

project(webdav-server)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -Wno-keyword-macro -fPIC -fPIE -march=znver2 -Wall -fstack-protector-all")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -std=c++20 -gdwarf-2 -O0 -pedantic -pedantic-errors")

set(APP_VERSION "1.00")

include_directories(
  source
)

add_executable(${PROJECT_NAME}.elf
  source/http/httplib.cpp
  source/server/webdav_server.cpp
  source/dbglogger.c
  source/main.cpp
)

target_compile_definitions(${PROJECT_NAME}.elf PRIVATE APP_VERSION=${APP_VERSION} CPPHTTPLIB_THREAD_POOL_COUNT=64 CPPHTTPLIB_OPENSSL_SUPPORT=1 CPPHTTPLIB_NO_ACCEPT4)

target_link_libraries(${PROJECT_NAME}.elf
  crypto
  ssl
  tinyxml2
  SceNotification
  SceSystemService
  SceSysCore
  kernel_sys
)
