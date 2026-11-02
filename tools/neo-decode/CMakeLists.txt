# SPDX-License-Identifier: GPL-3.0-or-later
# Reference decoder tool built against an external mbelib-neo checkout/build.
# Only configured when MBELIB_NEO_INCLUDE_DIRS and MBELIB_NEO_LIBRARY are given, e.g.
#   -DMBELIB_NEO_INCLUDE_DIRS="C:/src/mbelib-neo/include;C:/src/mbelib-neo/build/dev-release/include"
#   -DMBELIB_NEO_LIBRARY=C:/src/mbelib-neo/build/dev-release/mbe-neo-static.lib
set(MBELIB_NEO_INCLUDE_DIRS "" CACHE STRING "Include dirs of an mbelib-neo checkout (source include + build include)")
set(MBELIB_NEO_LIBRARY "" CACHE FILEPATH "Static mbelib-neo library")

if(MBELIB_NEO_INCLUDE_DIRS AND MBELIB_NEO_LIBRARY)
    add_executable(neo-decode main.cpp)
    target_include_directories(neo-decode PRIVATE ${MBELIB_NEO_INCLUDE_DIRS})
    target_compile_definitions(neo-decode PRIVATE MBE_STATIC)
    target_link_libraries(neo-decode PRIVATE ${MBELIB_NEO_LIBRARY})
    mbevocoder_apply_compiler_options(neo-decode)
    message(STATUS "mbevocoder: neo-decode enabled against ${MBELIB_NEO_LIBRARY}")
else()
    message(STATUS "mbevocoder: neo-decode disabled (set MBELIB_NEO_INCLUDE_DIRS and MBELIB_NEO_LIBRARY)")
endif()
