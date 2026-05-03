cmake_minimum_required(VERSION 3.14.0)
project(emu68-genet-driver VERSION 3.3)

string(TIMESTAMP CURRENT_DATE "(%d.%m.%Y)")
set(VERSTRING "$VER: ${PROJECT_NAME} ${PROJECT_VERSION} ${CURRENT_DATE}")

find_package(GIC400 REQUIRED)
find_package(Emu68Common REQUIRED)

# Set default install prefix to ./install subdirectory
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install" CACHE PATH "Install path" FORCE)
endif()

add_subdirectory(runtime-config)
add_subdirectory(genet.device)
add_subdirectory(tools)
