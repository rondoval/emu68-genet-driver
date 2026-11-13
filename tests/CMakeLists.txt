# Checks the bench rounds run on the Amiga (emu68-driver-stack/scripts/bench).
# Built with the driver, never installed.
add_executable(s2check s2check.c)
target_compile_options(s2check PRIVATE -Os -Wall -Wconversion -Wsign-conversion -Wshadow
    -Wmissing-prototypes -Wstrict-prototypes)
target_link_options(s2check PRIVATE -s)
target_compile_definitions(s2check PRIVATE
    VERSTAG="$VER: s2check ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} ${CURRENT_DATE}")
target_link_libraries(s2check PRIVATE Netdev::netdev_headers)
