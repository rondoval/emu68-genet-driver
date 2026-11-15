# The suite's programs (tests/README.md): GenetTest for the Amiga, the logic tier's for vamos on the host.
# Built with the driver, never installed.
set(GENET_TEST_WARNINGS -Wall -Wconversion -Wsign-conversion -Wshadow -Wmissing-prototypes -Wstrict-prototypes)

# GenetTest: the rig the hardware tiers run.
add_executable(GenetTest
    rig/genettest.c
    rig/rig.c
    rig/query.c
    rig/claim.c
    rig/mcast.c
    rig/events.c
    rig/miami.c
    common/kv.c
)
target_compile_options(GenetTest PRIVATE -Os ${GENET_TEST_WARNINGS})
target_link_options(GenetTest PRIVATE -s)
target_compile_definitions(GenetTest PRIVATE
    VERSTAG="$VER: GenetTest ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} ${CURRENT_DATE}")
target_link_libraries(GenetTest PRIVATE Netdev::netdev_headers)

# The logic tier's programs, run under vamos on the host.
add_subdirectory(logic)
