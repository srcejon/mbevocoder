# SPDX-License-Identifier: GPL-3.0-or-later
# Test tiers use ctest labels:
#   unit      fast, no assets
#   golden    needs committed frame goldens (corpus/golden)
#   assets    needs ${MBEVOCODER_TESTDATA_DIR} (fetched or captured)
#   hil       needs the AMBE-3000R on a serial port (MBEVOCODER_HIL_PORT)
#   bench     benchmarks, never gate

set(MBEVOCODER_HIL_PORT "" CACHE STRING "Serial port of an AMBE-3000R for hardware-in-the-loop tests (e.g. COM3, /dev/ttyUSB0)")

add_executable(test_version test_version.cpp)
target_link_libraries(test_version PRIVATE mbevocoder::mbevocoder)
mbevocoder_apply_compiler_options(test_version)
add_test(NAME version COMMAND test_version)
set_tests_properties(version PROPERTIES LABELS unit)

# The mbelib 1.x API (<mbelib/mbelib.h>), from C, against the mbelib-neo API on the same frames
add_executable(test_mbelib1_api test_mbelib1_api.c)
target_link_libraries(test_mbelib1_api PRIVATE mbevocoder::mbevocoder)
mbevocoder_apply_compiler_options(test_mbelib1_api)
add_test(NAME mbelib1_api COMMAND test_mbelib1_api)
set_tests_properties(mbelib1_api PROPERTIES LABELS unit)

# mbelib-neo 2.4's additions (encoder API, frame classifier, DV data, flags), from C with
# <mbelib-neo/mbelib.h> alone
add_executable(test_neo24_api test_neo24_api.c)
target_link_libraries(test_neo24_api PRIVATE mbevocoder::mbevocoder)
target_compile_definitions(test_neo24_api PRIVATE _USE_MATH_DEFINES)
mbevocoder_apply_compiler_options(test_neo24_api)
add_test(NAME neo24_api COMMAND test_neo24_api)
set_tests_properties(neo24_api PROPERTIES LABELS unit)

if(NOT CMAKE_CROSSCOMPILING)
    find_package(PkgConfig QUIET)
    configure_file(test_install.cmake.in test_install.cmake @ONLY)
    add_test(NAME installed_c_consumers COMMAND ${CMAKE_COMMAND}
        -Dconfig=$<CONFIG> -P ${CMAKE_CURRENT_BINARY_DIR}/test_install.cmake)
    set_tests_properties(installed_c_consumers PROPERTIES LABELS integration)
endif()

if(MBEVOCODER_BUILD_TOOLS AND MBEVOCODER_HIL_PORT)
    add_test(NAME hil_ambe3000_identify COMMAND ambe3000 --port ${MBEVOCODER_HIL_PORT} identify)
    set_tests_properties(hil_ambe3000_identify PROPERTIES LABELS hil)
endif()

if(MBEVOCODER_BUILD_TOOLS AND NOT CMAKE_CROSSCOMPILING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME cli_imbe COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cli.py" $<TARGET_FILE:mbe-cli>)
        set_tests_properties(cli_imbe PROPERTIES LABELS integration)
    endif()
endif()

foreach(t test_ecc test_frames test_threads test_encoder test_frontend test_fixedpoint test_dstar_quantizer test_dstar_analysis test_robustness)
    add_executable(${t} ${t}.cpp)
    target_link_libraries(${t} PRIVATE mbevocoder::mbevocoder)
    mbevocoder_apply_compiler_options(${t})
    add_test(NAME ${t} COMMAND ${t})
    set_tests_properties(${t} PROPERTIES LABELS unit)
endforeach()
target_include_directories(test_frontend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)   # encoder_internal.hpp
target_include_directories(test_fixedpoint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_include_directories(test_dstar_quantizer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_include_directories(test_dstar_analysis PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(test_dstar_analysis PRIVATE MBEVOCODER_DSTAR_PITCH_ANALYSIS=$<BOOL:${MBEVOCODER_DSTAR_PITCH_ANALYSIS}>
    MBEVOCODER_AMBE2450_PITCH_ANALYSIS=$<BOOL:${MBEVOCODER_AMBE2450_PITCH_ANALYSIS}> MBEVOCODER_IMBE_PITCH_ANALYSIS=$<BOOL:${MBEVOCODER_IMBE_PITCH_ANALYSIS}>)
# checks against AMBE-3000R-measured behaviour apply only when the library is built with it
target_compile_definitions(test_encoder PRIVATE MBEVOCODER_CHIP_MEASURED=$<BOOL:${MBEVOCODER_CHIP_MEASURED}>)
target_compile_definitions(test_ecc PRIVATE MBEVOCODER_CHIP_MEASURED=$<BOOL:${MBEVOCODER_CHIP_MEASURED}>)
target_compile_definitions(test_robustness PRIVATE MBEVOCODER_CHIP_MEASURED=$<BOOL:${MBEVOCODER_CHIP_MEASURED}>)

add_executable(test_tones_transcode test_tones_transcode.cpp)
target_link_libraries(test_tones_transcode PRIVATE mbevocoder::mbevocoder)
target_compile_definitions(test_tones_transcode PRIVATE MBEVOCODER_CHIP_MEASURED=$<BOOL:${MBEVOCODER_CHIP_MEASURED}>)
mbevocoder_apply_compiler_options(test_tones_transcode)
add_test(NAME tones_transcode COMMAND test_tones_transcode "${CMAKE_CURRENT_SOURCE_DIR}/data/tones")
set_tests_properties(tones_transcode PROPERTIES LABELS unit)

add_executable(test_golden_bits test_golden_bits.cpp)
target_link_libraries(test_golden_bits PRIVATE mbevocoder::mbevocoder)
mbevocoder_apply_compiler_options(test_golden_bits)
target_compile_definitions(test_golden_bits PRIVATE MBEVOCODER_CHIP_MEASURED=$<BOOL:${MBEVOCODER_CHIP_MEASURED}>)
# the mbelib-neo dumps live in the test-data bundle (MBEVOCODER_TESTDATA_DIR/neo) unless pointed elsewhere
mbevocoder_testdata_file(_neo_dumps neo)
set(MBELIB_NEO_DUMP_DIR "${_neo_dumps}" CACHE PATH "Directory of neo-decode --dump output for parity tests (default: MBEVOCODER_TESTDATA_DIR/neo)")
if(MBELIB_NEO_DUMP_DIR AND EXISTS "${MBELIB_NEO_DUMP_DIR}")
    add_test(NAME golden_bits COMMAND test_golden_bits "${PROJECT_SOURCE_DIR}/corpus/golden" "${CMAKE_CURRENT_SOURCE_DIR}/data" "${MBELIB_NEO_DUMP_DIR}")
else()
    add_test(NAME golden_bits COMMAND test_golden_bits "${PROJECT_SOURCE_DIR}/corpus/golden" "${CMAKE_CURRENT_SOURCE_DIR}/data")
endif()
set_tests_properties(golden_bits PROPERTIES LABELS golden)

add_executable(test_golden_frames test_golden_frames.cpp)
mbevocoder_apply_compiler_options(test_golden_frames)
add_test(NAME golden_frames COMMAND test_golden_frames "${PROJECT_SOURCE_DIR}/corpus/golden")
set_tests_properties(golden_frames PROPERTIES LABELS golden)

add_executable(test_decoder_parity test_decoder_parity.cpp)
target_link_libraries(test_decoder_parity PRIVATE mbevocoder::mbevocoder)
mbevocoder_apply_compiler_options(test_decoder_parity)
target_compile_definitions(test_decoder_parity PRIVATE MBEVOCODER_CHIP_MEASURED=$<BOOL:${MBEVOCODER_CHIP_MEASURED}>)
if(MBELIB_NEO_DUMP_DIR AND EXISTS "${MBELIB_NEO_DUMP_DIR}")
    add_test(NAME decoder_parity COMMAND test_decoder_parity "${PROJECT_SOURCE_DIR}/corpus/golden" "${MBELIB_NEO_DUMP_DIR}" 1.0 "${CMAKE_CURRENT_BINARY_DIR}/parity_tmp" $<IF:$<BOOL:${MBEVOCODER_CHIP_MEASURED}>,--no-dstar-pcm,--dstar-pcm> $<IF:$<BOOL:${MBEVOCODER_CHIP_MEASURED}>,--no-voiced-pcm,--voiced-pcm> $<IF:$<BOOL:${MBEVOCODER_CHIP_MEASURED}>,--no-imbe-meta,--imbe-meta>)
    set_tests_properties(decoder_parity PROPERTIES LABELS assets)
endif()
