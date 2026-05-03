cmake_minimum_required(VERSION 3.14.0)
project(genet-stats VERSION ${CMAKE_PROJECT_VERSION})

add_compile_options(-Os -Wall -Wconversion -Wsign-conversion -Wshadow -Wmissing-prototypes -Wstrict-prototypes)
add_link_options(-s)

add_executable(genet-stats genet-stats.c)

# Add shared include path for SANA-II headers
target_include_directories(genet-stats
    PRIVATE
	    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/genet-stats DESTINATION C)
