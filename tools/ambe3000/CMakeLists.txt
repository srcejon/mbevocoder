# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(ambe3000
    main.cpp
    ambe3000.cpp
    serial_port.cpp
)
target_include_directories(ambe3000 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
mbevocoder_apply_compiler_options(ambe3000)
if(APPLE)
    target_link_libraries(ambe3000 PRIVATE "-framework IOKit")
endif()
if(MBEVOCODER_INSTALL_TOOLS)
    install(TARGETS ambe3000 RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
