cmake_minimum_required(VERSION 3.14.0)
project(runtime-config VERSION ${CMAKE_PROJECT_VERSION})

add_compile_options(-Os -m68040 -fomit-frame-pointer -Wall -Wextra -DDEBUG)
file(GLOB_RECURSE common_sources src/*.c include/*.h)
add_library(runtime-config STATIC ${common_sources})

target_include_directories(runtime-config PUBLIC include/)

target_link_libraries(runtime-config common)