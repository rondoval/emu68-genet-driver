# The logic tier's programs (tests/README.md): the driver's own sources built as CLI programs for
# vamos on the host, never installed (emu68-common's emu68_logic_program()). Each includes the
# source under test, so its static functions are in reach.

# What every program but LogicPrefs adds to emu68-common's logic library: the unit, the driver's
# numbers by name, weak stand-ins for the driver functions a program does not link.
set(GENET_LOGIC_SUPPORT unit.c names.c stubs.c)

# genet_logic_program(<name> <sources>...): one program, with the driver's include paths.
function(genet_logic_program name)
    emu68_logic_program(${name}
        SOURCES ${ARGN}
        INCLUDES ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/genet.device/include
                 ${PROJECT_SOURCE_DIR}/runtime-config/include
        LIBRARIES GIC400::gic400_headers Netdev::netdev_headers
        OPTIONS ${GENET_TEST_WARNINGS})
endfunction()

genet_logic_program(LogicPrefs prefs.c)
genet_logic_program(LogicMcast mcast.c ${GENET_LOGIC_SUPPORT} ${PROJECT_SOURCE_DIR}/genet.device/src/bcmgenet-mac.c)
genet_logic_program(LogicNetdev netdev.c ${GENET_LOGIC_SUPPORT} ${PROJECT_SOURCE_DIR}/genet.device/src/netdev_api.c)
genet_logic_program(LogicSana2 sana2.c ${GENET_LOGIC_SUPPORT}
    ${PROJECT_SOURCE_DIR}/genet.device/src/sana2_api.c ${PROJECT_SOURCE_DIR}/genet.device/src/sana2_mcast.c
    ${PROJECT_SOURCE_DIR}/genet.device/src/bcmgenet-link.c ${PROJECT_SOURCE_DIR}/genet.device/src/sana2_tx.c)
genet_logic_program(LogicUnitTask unittask.c ${GENET_LOGIC_SUPPORT})
