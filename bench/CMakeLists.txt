# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(mbevocoder-bench bench.cpp)
target_link_libraries(mbevocoder-bench PRIVATE mbevocoder::mbevocoder)
mbevocoder_apply_compiler_options(mbevocoder-bench)
set_target_properties(mbevocoder-bench PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
if(MBEVOCODER_BUILD_TESTS)
    # ctest -L bench: never gates, prints the table; uses the speech corpus when fetched
    # (MBEVOCODER_TESTDATA_DIR/corpus), synthetic speech otherwise.
    mbevocoder_testdata_file(_bench_corpus corpus)
    add_test(NAME bench COMMAND mbevocoder-bench ${_bench_corpus} --seconds 60
                                             --json "${PROJECT_BINARY_DIR}/bench.json")
    set_tests_properties(bench PROPERTIES LABELS bench)
endif()
