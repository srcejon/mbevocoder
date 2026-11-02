# SPDX-License-Identifier: GPL-3.0-or-later
# Reference decoder built from an mbelib 1.x source checkout (https://github.com/szechyjs/mbelib),
# for tools/quality/decoder_lineage.py. Only configured when MBELIB1_SOURCE_DIR is given:
#   -DMBELIB1_SOURCE_DIR=C:/src/mbelib
# mbelib 1.x exports nothing from its DLL on Windows, so its sources are compiled in directly.
set(MBELIB1_SOURCE_DIR "" CACHE PATH "Source directory of an mbelib 1.x checkout")

if(MBELIB1_SOURCE_DIR AND EXISTS "${MBELIB1_SOURCE_DIR}/mbelib.c")
    set(M "${MBELIB1_SOURCE_DIR}")
    add_executable(mbelib1-decode main.c ${M}/mbelib.c ${M}/ambe3600x2400.c ${M}/ambe3600x2450.c
                   ${M}/imbe7200x4400.c ${M}/imbe7100x4400.c ${M}/ecc.c)
    target_include_directories(mbelib1-decode PRIVATE ${M})
    target_compile_definitions(mbelib1-decode PRIVATE _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
    if(NOT MSVC)
        target_link_libraries(mbelib1-decode PRIVATE m)
    endif()
    message(STATUS "mbevocoder: mbelib1-decode enabled from ${M}")
else()
    message(STATUS "mbevocoder: mbelib1-decode disabled (set MBELIB1_SOURCE_DIR to an mbelib 1.x checkout)")
endif()
