cmake_minimum_required(VERSION 3.14.0)
project(genet.device VERSION ${CMAKE_PROJECT_VERSION})

get_verstring(VERSTRING)

add_link_options(-ffreestanding -nostdlib -nostartfiles -s -Wl,-e,_doNotExecute)
add_compile_options(-Os -m68040 -Wall -Wextra -DDEBUG -fomit-frame-pointer)
add_compile_definitions(PRIVATE DEVICE_IDSTRING="${VERSTRING}" DEVICE_VERSION=${PROJECT_VERSION_MAJOR} DEVICE_REVISION=${PROJECT_VERSION_MINOR} DEVICE_NAME="${PROJECT_NAME}")

add_executable(genet.device
    src/device.c
    src/device_beginio.c
    src/device_abortio.c
    src/devtree_parse.c
    src/unit.c
    src/unit_task.c
    src/unit_commands.c
    src/unit_commands_mcast.c
    src/unit_io.c
    src/bcmgenet.c
    src/bcmgenet-tx.c
    src/phy.c
    src/phy_interface.c
    src/device_end.c
)

target_include_directories(genet.device PRIVATE include)
target_link_libraries(genet.device common runtime-config)

install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/genet.device DESTINATION .)
