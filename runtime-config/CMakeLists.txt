# SPDX-License-Identifier: GPL-2.0+

cmake_minimum_required(VERSION 3.14.0)
project(runtime-config VERSION ${CMAKE_PROJECT_VERSION})

# CPU/FPU/frame-pointer flags come from the stack toolchain (cmake/toolchain.cmake,
# knobs M68K_CPU / M68K_FPU) and must not be repeated here: a hardcoded -m68040 is
# emitted after CMAKE_C_FLAGS and would silently override M68K_CPU.
add_compile_options(
    -O3
    -Wall
    -Wextra
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wmissing-prototypes
    -Wstrict-prototypes
)

# Debug-output backend defines (EMU68_DEBUG_BACKEND, see emu68-common).
# Static lib: KPrintF stays unresolved here; genet.device resolves it at link.
emu68_debug_definitions()

file(GLOB_RECURSE runtime_config_sources src/*.c)
add_library(runtime-config STATIC ${runtime_config_sources})

target_include_directories(runtime-config PUBLIC include/)

target_link_libraries(runtime-config PUBLIC Emu68Common::common)