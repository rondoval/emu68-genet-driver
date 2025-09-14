cmake_minimum_required(VERSION 3.14.0)
project(emu68-genet-driver VERSION 1.3.1)

# Set default install prefix to ./install subdirectory
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install" CACHE PATH "Install path" FORCE)
endif()

add_subdirectory(common)
add_subdirectory(genet-hw)
add_subdirectory(genet.device)

# install(FILES ${CMAKE_SOURCE_DIR}/Install DESTINATION .)
# install(FILES ${CMAKE_SOURCE_DIR}/Install.info DESTINATION .)
