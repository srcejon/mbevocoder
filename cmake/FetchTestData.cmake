# SPDX-License-Identifier: GPL-3.0-or-later
# Large test assets (corpus audio, hardware-decoded PCM goldens) live outside the
# repository. They are described by corpus/manifest.json and either captured locally
# with the tools, or downloaded from ${MBEVOCODER_TESTDATA_URL} when MBEVOCODER_FETCH_TESTDATA=ON.
#
# Layout of ${MBEVOCODER_TESTDATA_DIR}:
#   corpus/<name>.wav               8 kHz s16 mono, level-normalised (see tools/corpus/prepare.py)
#   golden/<rate>/<name>.hw.wav     AMBE-3000R decode of its own frames
#   golden/<rate>/<name>.amb        (also committed under corpus/golden, they are small)
#
# Tests that need an asset call mbevocoder_testdata_file(<var> <relative path>) and are
# skipped (not failed) when it is missing.

set(MBEVOCODER_TESTDATA_BUNDLE "mbevocoder-testdata.tar.gz")

if(MBEVOCODER_FETCH_TESTDATA AND NOT EXISTS "${MBEVOCODER_TESTDATA_DIR}/MANIFEST.sha256")
    set(_url "${MBEVOCODER_TESTDATA_URL}/${MBEVOCODER_TESTDATA_BUNDLE}")
    set(_dst "${PROJECT_BINARY_DIR}/${MBEVOCODER_TESTDATA_BUNDLE}")
    message(STATUS "mbevocoder: fetching test data from ${_url}")
    file(DOWNLOAD "${_url}" "${_dst}" SHOW_PROGRESS STATUS _st TLS_VERIFY ON)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
        list(GET _st 1 _msg)
        message(WARNING "mbevocoder: test data download failed (${_msg}); asset-dependent tests will be skipped")
    else()
        file(MAKE_DIRECTORY "${MBEVOCODER_TESTDATA_DIR}")
        file(ARCHIVE_EXTRACT INPUT "${_dst}" DESTINATION "${MBEVOCODER_TESTDATA_DIR}")
    endif()
endif()

# Verify what is present against the manifest, if any. MANIFEST.sha256 lines are
# "<sha256>  <relative path>" as produced by tools/corpus/make_manifest.py.
if(EXISTS "${MBEVOCODER_TESTDATA_DIR}/MANIFEST.sha256")
    file(STRINGS "${MBEVOCODER_TESTDATA_DIR}/MANIFEST.sha256" _lines)
    set(_bad 0)
    foreach(_line IN LISTS _lines)
        if(_line MATCHES "^([0-9a-fA-F]+) +(.+)$")
            set(_expected "${CMAKE_MATCH_1}")
            set(_rel "${CMAKE_MATCH_2}")
            if(EXISTS "${MBEVOCODER_TESTDATA_DIR}/${_rel}")
                file(SHA256 "${MBEVOCODER_TESTDATA_DIR}/${_rel}" _actual)
                if(NOT _actual STREQUAL _expected)
                    message(WARNING "mbevocoder: test asset ${_rel} does not match manifest")
                    math(EXPR _bad "${_bad}+1")
                endif()
            endif()
        endif()
    endforeach()
    if(_bad EQUAL 0)
        message(STATUS "mbevocoder: test data at ${MBEVOCODER_TESTDATA_DIR} verified")
    endif()
else()
    message(STATUS "mbevocoder: no test data manifest at ${MBEVOCODER_TESTDATA_DIR}; asset-dependent tests will be skipped")
endif()

function(mbevocoder_testdata_file out_var rel)
    set(_p "${MBEVOCODER_TESTDATA_DIR}/${rel}")
    if(EXISTS "${_p}")
        set(${out_var} "${_p}" PARENT_SCOPE)
    else()
        set(${out_var} "" PARENT_SCOPE)
    endif()
endfunction()
