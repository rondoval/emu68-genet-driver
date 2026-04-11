# SPDX-License-Identifier: GPL-2.0+

cmake_minimum_required(VERSION 3.14.0)
project(runtime-config VERSION ${CMAKE_PROJECT_VERSION})

add_compile_options(-Os -m68040 -fomit-frame-pointer -Wall -Wextra -DDEBUG)
file(GLOB_RECURSE runtime_config_sources src/*.c)
add_library(runtime-config STATIC ${runtime_config_sources})

target_include_directories(runtime-config PUBLIC include/)

target_link_libraries(runtime-config PUBLIC Emu68Common::common)