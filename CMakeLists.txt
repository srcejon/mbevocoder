# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)

project(mbevocoder
    VERSION 3.0.0
    DESCRIPTION "AMBE, AMBE+2 and IMBE vocoder encoder/decoder library"
    HOMEPAGE_URL "https://github.com/srcejon/mbevocoder"
    LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Default to Release only for a build of this project itself: as a subproject, the parent's
# (possibly empty) CMAKE_BUILD_TYPE is the parent's to choose.
if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(MBEVOCODER_IS_TOP_LEVEL ${PROJECT_IS_TOP_LEVEL})

option(MBEVOCODER_BUILD_TOOLS    "Build command-line tools (AMBE-3000R capture, mbe-cli)" ${MBEVOCODER_IS_TOP_LEVEL})
option(MBEVOCODER_BUILD_TESTS    "Build unit and regression tests" ${MBEVOCODER_IS_TOP_LEVEL})
option(MBEVOCODER_BUILD_BENCH    "Build benchmarks" OFF)
option(MBEVOCODER_INSTALL_TOOLS  "Install the command-line tools (mbe-cli, ambe3000) with the library" OFF)
option(MBEVOCODER_FETCH_TESTDATA "Download large test assets (corpus audio, PCM goldens) at configure time" OFF)
option(MBEVOCODER_ENABLE_ASAN    "Enable AddressSanitizer" OFF)
option(MBEVOCODER_ENABLE_UBSAN   "Enable UndefinedBehaviorSanitizer" OFF)
option(MBEVOCODER_ENABLE_TSAN    "Enable ThreadSanitizer" OFF)
option(MBEVOCODER_STRICT_FP      "Disable FMA contraction and fast-math for divergence debugging" OFF)
option(MBEVOCODER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(MBEVOCODER_DSTAR_WEIGHTED_QUANTIZER "D-STAR voice encoder: speech-weighted spectral quantizer (docs/encoder-quality.md). OFF restores the D-STAR encoder of mbevocoder development before 24 September 2026" ON)
option(MBEVOCODER_DSTAR_PITCH_ANALYSIS "D-STAR voice encoder: floating-point analysis, continuity-aware pitch correction and band-energy amplitudes (docs/encoder-quality.md). OFF restores the earlier D-STAR encoder" ON)
option(MBEVOCODER_AMBE2450_PITCH_ANALYSIS "AMBE+2 voice encoder: the same floating-point pitch correction and band-energy amplitudes (docs/encoder-quality.md). OFF restores the earlier AMBE+2 encoder" ON)
option(MBEVOCODER_IMBE_PITCH_ANALYSIS "IMBE voice encoder: the same pitch correction, voicing and amplitudes re-analysed at a moved pitch (docs/encoder-quality.md). OFF restores the earlier IMBE encoder" ON)
option(MBEVOCODER_ENCODER_INPUT_GAIN "Voice encoders: block-floating-point gain in front of the fixed-point analysis, taken off again in the quantisers; keeps quiet input from losing pitch and voicing (docs/encoder-quality.md 'Quiet input'). OFF restores the encoders of before 11 October 2026 bit for bit" ON)
option(MBEVOCODER_IMBE_VOICING_THRESHOLD "IMBE voice encoder: the standard's voicing threshold x1.75 on every band, which corrects the analysis' bias toward unvoiced (docs/encoder-quality.md 'Voicing threshold'; ESTOI +0.005 on unseen recordings through our decoder and JMBE). Off by default: a blind listening check heard no gain (docs/encoder-quality.md)" OFF)
option(MBEVOCODER_CHIP_MEASURED "Decode as the AMBE-3000R was measured to (docs/decoder-quality.md, tone-frames.md, frontend.md): the D-STAR fundamental mapping, harmonic count, shape prediction and memoryless gain; tone amplitude laws and D-STAR dual tones; mixed-excitation voicing tables and the fixed band grid of the unvoiced indices 18-31; geometric amplitude ramps, smoothed phases, an 80 Hz output high-pass and the AMBE-3000R's output level; the AMBE+2 silence-frame model, erasures and unusable tone frames played as repeats, and the AMBE-3000R's order of b3/b4's low bits (docs/bit-order.md); frame repeats that keep the error-rate and noise state running; the IMBE 7200 frame decoder counting Golay parity-bit errors for the repeat and mute rules (docs/decoder-quality.md). OFF restores mbelib-neo's decoder bit for bit (the neo-compat preset, a verification configuration, not a build to ship)" ON)
option(MBEVOCODER_FLOAT_CLIP "Clip the float synthesis output at 95 % of full scale on mbelib's 1/7 scale, as mbelib-neo does (mbelib 1.x left the float path unclipped; the 16-bit conversion always clips). Off by default: at mbelib's output level (MBEVOCODER_CHIP_MEASURED off) ordinary speech clipped on 2-3 % of samples (docs/decoder-quality.md)" OFF)
set(MBEVOCODER_EXTRA_DEFINITIONS "" CACHE STRING "Extra compile definitions for the library (semicolon list), for the synthesis experiment knobs in src/decoder (tools/quality/synth_diag.py)")
set(MBEVOCODER_TESTDATA_URL "https://github.com/srcejon/mbevocoder/releases/download/testdata-v1"
    CACHE STRING "Base URL for the large test asset bundle (see cmake/FetchTestData.cmake)")
set(MBEVOCODER_TESTDATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/testdata"
    CACHE PATH "Directory holding large test assets (fetched or captured locally; git-ignored)")

include(CompilerOptions)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
file(GLOB MBEVOCODER_IMBE_VOCODER_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/analysis/imbe_vocoder/*.cc)
configure_file(include/mbevocoder/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/include/mbevocoder/version.h" @ONLY)

add_library(mbevocoder STATIC
    src/version.cpp
    src/core/result.cpp
    src/compat/mbelib1.cpp
    src/compat/neo_encoder.cpp
    src/bits/ecc.cpp
    src/bits/ecc_api.cpp
    src/bits/ambe_frame.cpp
    src/bits/imbe_frame.cpp
    src/bits/layouts.cpp
    src/params/fields.cpp
    # Decoder: vendored from mbelib-neo 2.1.0 (GPL-2.0-or-later), see NOTICE.md
    src/decoder/mbelib.c
    src/decoder/mbe_adaptive.c
    src/decoder/mbe_unvoiced_fft.c
    src/decoder/ambe_common.c
    src/decoder/ambe3600x2400.c
    src/decoder/ambe3600x2450.c
    src/decoder/imbe7200x4400.c
    src/decoder/imbe7100x4400.c
    src/decoder/pffft/pffft.c
    src/decoder/pffft/fftpack.c
    # Encoder
    src/encoder/encoder.cpp
    src/encoder/ambe_quant.cpp
    src/encoder/dstar_quantizer.cpp
    src/encoder/dstar_analysis.cpp
    src/encoder/tone.cpp
    src/encoder/tonedetect.cpp
    src/encoder/vad.cpp
    src/encoder/noisesuppress.cpp
    src/encoder/echocancel.cpp
    src/transcode/transcoder.cpp
    # Analysis: vendored from OP25's imbe_vocoder (Pavel Yazev, GPL-3.0-or-later)
    ${MBEVOCODER_IMBE_VOCODER_SOURCES}
)
add_library(mbevocoder::mbevocoder ALIAS mbevocoder)

target_include_directories(mbevocoder
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/decoder
        ${CMAKE_CURRENT_SOURCE_DIR}/src/decoder/pffft
)
target_compile_definitions(mbevocoder
    PUBLIC  MBE_STATIC          # always a static library; keeps mbelib-neo's MBE_API macro happy
    PRIVATE MBE_BUILDING MBEVOCODER_CHIP_MEASURED=$<BOOL:${MBEVOCODER_CHIP_MEASURED}>
            MBEVOCODER_DSTAR_WEIGHTED_QUANTIZER=$<BOOL:${MBEVOCODER_DSTAR_WEIGHTED_QUANTIZER}>
            MBEVOCODER_DSTAR_PITCH_ANALYSIS=$<BOOL:${MBEVOCODER_DSTAR_PITCH_ANALYSIS}>
            MBEVOCODER_AMBE2450_PITCH_ANALYSIS=$<BOOL:${MBEVOCODER_AMBE2450_PITCH_ANALYSIS}>
            MBEVOCODER_IMBE_PITCH_ANALYSIS=$<BOOL:${MBEVOCODER_IMBE_PITCH_ANALYSIS}>
            MBEVOCODER_IMBE_VOICING_THRESHOLD=$<BOOL:${MBEVOCODER_IMBE_VOICING_THRESHOLD}>
            MBEVOCODER_ENCODER_INPUT_GAIN=$<BOOL:${MBEVOCODER_ENCODER_INPUT_GAIN}>
            MBEVOCODER_FLOAT_CLIP=$<BOOL:${MBEVOCODER_FLOAT_CLIP}>
            ${MBEVOCODER_EXTRA_DEFINITIONS})
target_compile_features(mbevocoder PUBLIC cxx_std_17)
mbevocoder_apply_compiler_options(mbevocoder)
option(MBEVOCODER_ENABLE_SIMD "Enable SSE2/NEON paths in the decoder and PFFFT" ON)
if(NOT MBEVOCODER_ENABLE_SIMD)
    target_compile_definitions(mbevocoder PRIVATE PFFFT_SIMD_DISABLE=1)
else()
    target_compile_definitions(mbevocoder PRIVATE MBEVOCODER_ENABLE_SIMD=1)
endif()
# Vendored decoder: its constant tables are written as double literals assigned to float.
file(GLOB MBEVOCODER_DECODER_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/decoder/*.c ${CMAKE_CURRENT_SOURCE_DIR}/src/decoder/pffft/*.c)
set_source_files_properties(${MBEVOCODER_IMBE_VOCODER_SOURCES} PROPERTIES
    COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/wd4244;/wd4245;/wd4456;/wd4457;/wd4458;/wd4459;/wd4701;/wd4310;/wd4267,-Wno-conversion;-Wno-sign-conversion;-Wno-shadow;-Wno-unused-parameter;-Wno-unused-but-set-variable>")
set_source_files_properties(${MBEVOCODER_DECODER_SOURCES} PROPERTIES
    COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/wd4305;/wd4244,-Wno-conversion;-Wno-float-conversion>"
    COMPILE_DEFINITIONS "_USE_MATH_DEFINES")
# Vendored third-party FFT: compile without warnings rather than editing it (appended, so the
# decoder sources' options above do not replace it).
set_property(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/decoder/pffft/pffft.c ${CMAKE_CURRENT_SOURCE_DIR}/src/decoder/pffft/fftpack.c
    APPEND PROPERTY COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")
# Always static (VERSION / SOVERSION would mean nothing); the debug postfix lets Debug and Release
# installs live side by side (the exported targets pick the right one per configuration).
set_target_properties(mbevocoder PROPERTIES
    OUTPUT_NAME mbevocoder
    EXPORT_NAME mbevocoder
    DEBUG_POSTFIX d)
if(NOT MSVC)
    target_link_libraries(mbevocoder PUBLIC m)
endif()

# A C-only consumer uses the C linker even though this archive contains C++.
# Export the extra libraries the configured C++ compiler normally adds (libstdc++
# or libc++, for example), without hard-coding a compiler/platform's runtime.
set(MBEVOCODER_CXX_RUNTIME_LIBRARIES ${CMAKE_CXX_IMPLICIT_LINK_LIBRARIES})
if(CMAKE_C_IMPLICIT_LINK_LIBRARIES)
    list(REMOVE_ITEM MBEVOCODER_CXX_RUNTIME_LIBRARIES ${CMAKE_C_IMPLICIT_LINK_LIBRARIES})
endif()
list(REMOVE_DUPLICATES MBEVOCODER_CXX_RUNTIME_LIBRARIES)
if(MBEVOCODER_CXX_RUNTIME_LIBRARIES)
    target_link_libraries(mbevocoder INTERFACE
        "$<$<LINK_LANGUAGE:C>:${MBEVOCODER_CXX_RUNTIME_LIBRARIES}>")
endif()

# pkg-config's --static output must carry the same dependencies for C callers.
set(MBEVOCODER_PC_PRIVATE_LIBS "")
foreach(_lib IN LISTS MBEVOCODER_CXX_RUNTIME_LIBRARIES)
    if(IS_ABSOLUTE "${_lib}")
        string(APPEND MBEVOCODER_PC_PRIVATE_LIBS " \"${_lib}\"")
    else()
        string(APPEND MBEVOCODER_PC_PRIVATE_LIBS " -l${_lib}")
    endif()
endforeach()
if(NOT MSVC AND NOT "m" IN_LIST MBEVOCODER_CXX_RUNTIME_LIBRARIES)
    string(APPEND MBEVOCODER_PC_PRIVATE_LIBS " -lm")
endif()
string(STRIP "${MBEVOCODER_PC_PRIVATE_LIBS}" MBEVOCODER_PC_PRIVATE_LIBS)

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
if(MBEVOCODER_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(MBEVOCODER_BUILD_TESTS)
    enable_testing()
    include(FetchTestData)
    add_subdirectory(tests)
endif()

if(MBEVOCODER_BUILD_BENCH)
    add_subdirectory(bench)
endif()

# ---------------------------------------------------------------------------
# Install / export: our own package (mbevocoder) plus an mbe-neo compatible one so
# consumers written for mbelib-neo (dsdcc's FindLibMbeNeo, which SDRangel uses) work unchanged.
# ---------------------------------------------------------------------------
install(TARGETS mbevocoder EXPORT mbevocoderTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/mbevocoder include/mbelib include/mbelib-neo DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/mbevocoder/version.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mbevocoder)

install(EXPORT mbevocoderTargets NAMESPACE mbevocoder:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mbevocoder)
configure_package_config_file(cmake/mbevocoderConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/mbevocoderConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mbevocoder)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/mbevocoderConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/mbevocoderConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/mbevocoderConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mbevocoder)

# mbe-neo compatibility package: find_package(mbe-neo 2 CONFIG) resolves to this library.
configure_package_config_file(cmake/mbe-neoConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/mbe-neoConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mbe-neo)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/mbe-neoConfigVersion.cmake"
    VERSION 2.99.0 COMPATIBILITY AnyNewerVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/mbe-neoConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/mbe-neoConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mbe-neo)

# pkg-config files locate the install from their own directory (${pcfiledir}), so a relocated
# install still works; an absolute CMAKE_INSTALL_LIBDIR keeps the absolute prefix.
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set(MBEVOCODER_PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
else()
    string(REGEX MATCHALL "[^/]+" _pc_parts "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
    set(MBEVOCODER_PC_PREFIX "\${pcfiledir}")
    foreach(_part IN LISTS _pc_parts)
        string(APPEND MBEVOCODER_PC_PREFIX "/..")
    endforeach()
endif()
configure_file(cmake/mbevocoder.pc.in "${CMAKE_CURRENT_BINARY_DIR}/mbevocoder.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/mbevocoder.pc" DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
configure_file(cmake/libmbe-neo.pc.in "${CMAKE_CURRENT_BINARY_DIR}/libmbe-neo.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libmbe-neo.pc" DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# Licence and attribution texts (GPL-3.0-or-later; the third-party notices of what is vendored).
install(FILES LICENSE NOTICE.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY LICENSES/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/LICENSES)
