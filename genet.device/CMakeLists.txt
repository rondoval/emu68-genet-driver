cmake_minimum_required(VERSION 3.14.0)
project(genet.device VERSION ${CMAKE_PROJECT_VERSION})

# Toolchain and compile options
add_link_options(
    -nostdlib
    -nostartfiles
    -s
)

# CPU/FPU/frame-pointer flags come from the stack toolchain (cmake/toolchain.cmake,
# knobs M68K_CPU / M68K_FPU) and must not be repeated here: a hardcoded -m68040 is
# emitted after CMAKE_C_FLAGS and would silently override M68K_CPU.
add_compile_options(
    -O3
    -ffreestanding
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

# Cache-op flavor (EMU68_FORCE_LVO_CACHE_OPS, see emu68-common cache_ops.h).
emu68_cache_ops_definitions()

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
    src/bcmgenet-isr.c
    src/bcmgenet_mib.c
    src/phy.c
    src/phy_bcm54xx.c
    src/phy_interface.c
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
# Layout contract (entry at offset 0, RT_ENDSKIP, no writable sections) belongs to the
# linker, not to source order -- see Emu68CommonModuleLayout.cmake.
emu68_module_layout(genet.device)

# Exec interrupt servers answer with the Z flag, not D0 (exec.library/AddIntServer);
# a compiler epilogue that ends in MOVE rather than MOVEM silently overwrites it.
# emu68_isr_z_check() slices each server out of the LINKED module (by the section
# EMU68_INTSERVER() puts it in), so a server's TU is an ordinary LTO'd TU.
emu68_enable_lto(genet.device)
# bcmgenet-isr.c is one file-scope asm() block, so there is no IR for LTO to carry and
# which partition it would land in is unspecified.  It is a TU of its own precisely so
# that this opt-out covers the server alone and not bcmgenet-irq.c's helpers with it.
emu68_lto_keep_real_objects(genet.device
    src/bcmgenet-isr.c
)
emu68_isr_z_check(genet.device SERVERS bcmgenet_isr0)

install(
    PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/genet.device
    DESTINATION DEVS/Networks
)
