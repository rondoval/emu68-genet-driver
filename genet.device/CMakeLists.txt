cmake_minimum_required(VERSION 3.14.0)
project(genet.device VERSION ${CMAKE_PROJECT_VERSION})

# Toolchain and compile options
add_link_options(
    -nostdlib
    -nostartfiles
    -s
    -Wl,-e,_doNotExecute
)

add_compile_options(
    -O3
    -m68040
    -ffreestanding
    -fomit-frame-pointer
    -Wall
    -Wextra
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wmissing-prototypes
    -Wstrict-prototypes
)

# Debug-output backend defines (EMU68_DEBUG_BACKEND, see emu68-common).
emu68_debug_definitions()

# cache_ops.h LVO-fallback define (EMU68_FORCE_LVO_CACHE_OPS, see emu68-common).
emu68_cache_ops_definitions()

# Sources
add_executable(genet.device
    src/device.c
    src/devtree_parse.c
    src/unit.c
    src/unit_task.c
    src/netdev_api.c
    src/bcmgenet.c
    src/bcmgenet-rx.c
    src/bcmgenet-tx.c
    src/bcmgenet-dma.c
    src/bcmgenet-mac.c
    src/bcmgenet-link.c
    src/bcmgenet-irq.c
    src/bcmgenet-mib.c
    src/phy.c
    src/phy_mdio.c
    src/phy_bcm54xx.c
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

target_include_directories(genet.device
    PRIVATE
        include
)

target_link_libraries(genet.device
    PRIVATE
        runtime-config
        Emu68Common::common
        GIC400::gic400_headers
        Netdev::netdev_headers
)

# ROM-able: no writable .data/.bss may sneak in. For the serial backend this
# instead links debug.lib (+ __divsi3 glue) and skips the ROM check.
emu68_debug_backend_finalize(genet.device ROMABLE)

install(
    PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/genet.device
    DESTINATION DEVS/Networks
)
