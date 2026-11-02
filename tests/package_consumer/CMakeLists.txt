# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)
project(mbevocoder_c_consumer LANGUAGES C)

enable_testing()
find_package(mbevocoder CONFIG REQUIRED)
find_package(mbe-neo 2 CONFIG REQUIRED)
foreach(kind mbevocoder neo_static neo_shared)
    add_executable(${kind} main.c)
    add_test(NAME ${kind} COMMAND ${kind})
endforeach()
target_link_libraries(mbevocoder PRIVATE mbevocoder::mbevocoder)
target_link_libraries(neo_static PRIVATE mbe_neo::mbe_static)
target_link_libraries(neo_shared PRIVATE mbe_neo::mbe_shared)

if(MBEVOCODER_TEST_PKG_CONFIG)
    find_package(PkgConfig REQUIRED)
    # The parent test stages the install with --prefix. Override the configured
    # prefix in its .pc file, just as a packaging/staging consumer would.
    execute_process(COMMAND "${PKG_CONFIG_EXECUTABLE}"
        "--define-variable=prefix=${CMAKE_PREFIX_PATH}" --static --libs "${MBEVOCODER_TEST_PC_FILE}"
        OUTPUT_VARIABLE link_flags OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${PKG_CONFIG_EXECUTABLE}"
        "--define-variable=prefix=${CMAKE_PREFIX_PATH}" --static --cflags "${MBEVOCODER_TEST_PC_FILE}"
        OUTPUT_VARIABLE compile_flags OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    separate_arguments(link_flags UNIX_COMMAND "${link_flags}")
    separate_arguments(compile_flags UNIX_COMMAND "${compile_flags}")
    add_executable(pkg_config main.c)
    target_compile_options(pkg_config PRIVATE ${compile_flags})
    target_link_libraries(pkg_config PRIVATE ${link_flags})
    add_test(NAME pkg_config COMMAND pkg_config)
endif()
