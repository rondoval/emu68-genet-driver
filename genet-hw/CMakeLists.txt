cmake_minimum_required(VERSION 3.14.0)
project(genet-hw VERSION 1.3.1)

add_compile_options(-O3 -Wall -Wextra -DDEBUG)
file(GLOB_RECURSE common_sources src/*.c include/*.h)
add_library(genet-hw STATIC ${common_sources})
target_include_directories(genet-hw PRIVATE include)
target_include_directories(genet-hw INTERFACE include/bcmgenet.h)
