# SPDX-License-Identifier: GPL-3.0-or-later
# Drivers for tools/quality/dvsi_decoders.py (and decoder_lineage comparisons with mbelib-neo 2.x):
#   dvsisoft  DVSI soft-decision vectors -> mbevocoder's soft decode, and soft frames for neodec
#   neodec    an mbelib-neo 2.x checkout's decoder on mbe-cli frbits / dvsisoft output
#   neoenc    an mbelib-neo 2.4 checkout's encoders, frames as mbe-cli frbits text (mbe-cli frpack packs them)
# Not part of the library build:
#   cmake -S tools/quality/dvsi_harness -B build/neo_now/b -DNEO_DIR=<mbelib-neo checkout, built in NEO_DIR/b>
#   cmake --build build/neo_now/b --config Release
cmake_minimum_required(VERSION 3.21)
project(dvsi_harness LANGUAGES C CXX)
get_filename_component(MBEVOCODER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(MBEVOCODER_BUILD "${MBEVOCODER_ROOT}/build/vs2026" CACHE PATH "mbevocoder build tree (static library and generated headers)")
set(MBEVOCODER_LIBRARY "${MBEVOCODER_BUILD}/Release/mbevocoder.lib" CACHE FILEPATH "mbevocoder static library")
set(NEO_DIR "" CACHE PATH "mbelib-neo 2.x checkout, configured and built in NEO_DIR/b")
set(NEO_LIBRARY "${NEO_DIR}/b/Release/mbe-neo-static.lib" CACHE FILEPATH "mbelib-neo static library")

add_executable(dvsisoft dvsisoft.cpp)
target_include_directories(dvsisoft PRIVATE ${MBEVOCODER_ROOT}/include ${MBEVOCODER_BUILD}/include)
target_compile_definitions(dvsisoft PRIVATE MBE_STATIC)
target_link_libraries(dvsisoft PRIVATE ${MBEVOCODER_LIBRARY})

if(NEO_DIR)
    add_executable(neodec neodec.c)
    target_include_directories(neodec PRIVATE ${NEO_DIR}/include ${NEO_DIR}/b/include)
    target_compile_definitions(neodec PRIVATE MBE_STATIC)
    target_link_libraries(neodec PRIVATE ${NEO_LIBRARY})
    # mbelib-neo 2.4's encoders (tools/quality/neo_encoders.py); needs a 2.4 or later checkout
    add_executable(neoenc neoenc.c)
    target_include_directories(neoenc PRIVATE ${NEO_DIR}/include ${NEO_DIR}/b/include)
    target_compile_definitions(neoenc PRIVATE MBE_STATIC _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(neoenc PRIVATE ${NEO_LIBRARY})
endif()
