cmake_minimum_required(VERSION 3.14.0)
project(genet.device VERSION ${CMAKE_PROJECT_VERSION})

# Toolchain and compile options
add_link_options(
    -ffreestanding
    -nostdlib
    -nostartfiles
    -s
    -Wl,-e,_doNotExecute
)

add_compile_options(
    -O3
    -m68040
    -Wall
    -Wextra
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wmissing-prototypes
    -Wstrict-prototypes
    -fomit-frame-pointer
    -fno-tree-loop-distribute-patterns
)

# Debug-output backend defines (EMU68_DEBUG_BACKEND, see emu68-common).
emu68_debug_backend_definitions()

# Sources
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
    src/bcmgenet-irq.c
    src/bcmgenet_mib.c
    src/phy.c
    src/phy_interface.c
    src/device_end.c
)

target_compile_definitions(genet.device
    PRIVATE
        DEVICE_IDSTRING="${VERSTRING}"
        DEVICE_VERSION=${PROJECT_VERSION_MAJOR}
        DEVICE_REVISION=${PROJECT_VERSION_MINOR}
        DEVICE_NAME="${PROJECT_NAME}"
)

# Add shared include path for SANA-II headers
target_include_directories(genet.device
    PRIVATE
	    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

option(GENET_QUICK_READ  "Enable inline quick path for CMD_READ" ON)

if(GENET_QUICK_READ)
    target_compile_definitions(genet.device PRIVATE GENET_QUICK_READ)
endif()

target_include_directories(genet.device
    PRIVATE
        include
)

target_link_libraries(genet.device
    PRIVATE
        runtime-config
        Emu68Common::common
        GIC400::gic400_headers
)

# ROM-able: no writable .data/.bss may sneak in. For the serial backend this
# instead links debug.lib (+ __divsi3 glue) and skips the ROM check.
emu68_debug_backend_finalize(genet.device ROMABLE)

install(
    PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/genet.device
    DESTINATION DEVS/Networks
)
