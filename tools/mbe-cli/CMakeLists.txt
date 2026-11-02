# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(mbe-cli main.cpp)
target_link_libraries(mbe-cli PRIVATE mbevocoder::mbevocoder)
target_include_directories(mbe-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../ambe3000 ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
mbevocoder_apply_compiler_options(mbe-cli)
if(MBEVOCODER_INSTALL_TOOLS)
    install(TARGETS mbe-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
