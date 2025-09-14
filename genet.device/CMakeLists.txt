cmake_minimum_required(VERSION 3.14.0)
project(genet.device VERSION 1.3.1)

add_link_options(-ffreestanding -nostdlib -nostartfiles -s)
add_compile_options(-O3 -Wall -Wextra -DDEBUG)

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
