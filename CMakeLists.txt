cmake_minimum_required(VERSION 3.31)
project(ngx_http_adaptive_rl_module C)

set(CMAKE_C_STANDARD 11)

if(NOT DEFINED ENV{NGINX_SRC})
    message(FATAL_ERROR "Environment variable NGINX_SRC is not set. Please set it to the Nginx source directory.")
endif()

set(NGINX_SRC_DIR $ENV{NGINX_SRC})

include_directories(${NGINX_SRC_DIR}/src/core)
include_directories(${NGINX_SRC_DIR}/src/http)
include_directories(${NGINX_SRC_DIR}/src/event)
include_directories(${NGINX_SRC_DIR}/src/os/unix)
include_directories(${NGINX_SRC_DIR}/objs)
include_directories(/opt/homebrew/include)

add_library(ngx_http_adaptive_rl_module SHARED
        ngx_http_adaptive_rl_module.c)
