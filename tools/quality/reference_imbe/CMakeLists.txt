# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)
project(reference_pack LANGUAGES CXX)
get_filename_component(MBEVOCODER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(MBEVOCODER_LIBRARY "${MBEVOCODER_ROOT}/build/vs2026/Release/mbevocoder.lib" CACHE FILEPATH "Built mbevocoder static library")
add_executable(reference-pack pack.cpp)
target_include_directories(reference-pack PRIVATE "${MBEVOCODER_ROOT}/include" "${MBEVOCODER_ROOT}/build/vs2026/include")
target_compile_definitions(reference-pack PRIVATE MBE_STATIC)
target_link_libraries(reference-pack PRIVATE "${MBEVOCODER_LIBRARY}")
