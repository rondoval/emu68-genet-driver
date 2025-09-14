cmake_minimum_required(VERSION 3.14.0)
project(runtime-config VERSION 1.3.1)

add_compile_options(-O3 -Wall -Wextra -DDEBUG)
file(GLOB_RECURSE common_sources src/*.c include/*.h)
add_library(runtime-config STATIC ${common_sources})

target_include_directories(runtime-config PUBLIC include/)

target_link_libraries(runtime-config common)