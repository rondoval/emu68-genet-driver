# The logic tier's programs (tests/README.md): the driver's own sources built as CLI programs for
# vamos on the host, never installed. Each includes the source under test, so its static functions
# are in reach. No debug sink (Kprintf writes to the PiStorm's debug port, which vamos does not
# have), and the exec LVO cache operations (vamos runs no LINE-F opcode).

# The support every program but LogicPrefs links: fake exec and utility, the unit, names, the step
# loop, weak stubs.
set(GENET_LOGIC_SUPPORT fakelib.c fakeexec.c unit.c names.c steps.c stubs.c)

# genet_logic_program(<name> <sources>...): one program, with kv.c and the driver's include paths.
function(genet_logic_program name)
    add_executable(${name} ${ARGN} ${PROJECT_SOURCE_DIR}/tests/common/kv.c)
    target_compile_options(${name} PRIVATE -O2 -Wextra ${GENET_TEST_WARNINGS})
    target_compile_definitions(${name} PRIVATE EMU68_FORCE_LVO_CACHE_OPS)
    target_include_directories(${name} PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/genet.device/include
        ${PROJECT_SOURCE_DIR}/runtime-config/include
        ${PROJECT_SOURCE_DIR}/tests/common
    )
    target_link_libraries(${name} PRIVATE Emu68Common::common GIC400::gic400_headers Netdev::netdev_headers)
endfunction()

genet_logic_program(LogicPrefs prefs.c)
genet_logic_program(LogicMcast mcast.c ${GENET_LOGIC_SUPPORT} ${PROJECT_SOURCE_DIR}/genet.device/src/bcmgenet-mac.c)
genet_logic_program(LogicNetdev netdev.c ${GENET_LOGIC_SUPPORT} ${PROJECT_SOURCE_DIR}/genet.device/src/netdev_api.c)
genet_logic_program(LogicSana2 sana2.c ${GENET_LOGIC_SUPPORT}
    ${PROJECT_SOURCE_DIR}/genet.device/src/sana2_api.c ${PROJECT_SOURCE_DIR}/genet.device/src/sana2_mcast.c
    ${PROJECT_SOURCE_DIR}/genet.device/src/bcmgenet-link.c ${PROJECT_SOURCE_DIR}/genet.device/src/sana2_tx.c)
genet_logic_program(LogicUnitTask unittask.c ${GENET_LOGIC_SUPPORT})
