cmake_minimum_required(VERSION 3.14.0)
project(emu68-genet-driver VERSION 4.4)

string(TIMESTAMP CURRENT_DATE "(%d.%m.%Y)")
if(EMU68_BUILD_STAMP)   # local-build marker from the stack's build.sh; empty in releases
    string(APPEND CURRENT_DATE " ${EMU68_BUILD_STAMP}")
endif()
set(VERSTRING "$VER: ${PROJECT_NAME} ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} ${CURRENT_DATE}")

find_package(GIC400 1.3 REQUIRED)
find_package(Emu68Common 2.3 REQUIRED) # 2.3: prefs_open()
find_package(Netdev 1.0 REQUIRED) # the netdev driver ABI header (lwip-amiga)

# Set default install prefix to ./install subdirectory
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install" CACHE PATH "Install path" FORCE)
endif()

add_subdirectory(runtime-config)
add_subdirectory(genet.device)
