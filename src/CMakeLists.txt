# libhdlc - bit-level HDLC framing, CRC-16, RFC 1662 byte framing, locked ring buffer. C11.
#
# Consumers:   add_subdirectory(path/to/libhdlc/src)   then   target_link_libraries(app PRIVATE hdlc::hdlc)
# Developers:  cmake -S src -B build && cmake --build build && ctest --test-dir build
#
# The Arduino IDE does not use this file; it reads library.properties at the repository root and
# compiles everything under src/.
cmake_minimum_required(VERSION 3.13)
project(libhdlc VERSION 0.1.0 LANGUAGES C)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(HDLC_IS_TOP_LEVEL ON)
else()
    set(HDLC_IS_TOP_LEVEL OFF)
endif()

option(HDLC_BUILD_TESTS "Build the CTest unit tests" ${HDLC_IS_TOP_LEVEL})
option(HDLC_WERROR "Treat every warning as an error (CI only)" OFF)

include(cmake/house_standard.cmake)

add_library(hdlc STATIC
    libhdlc/hdlc.c
    libhdlc/byte_frame.c
    libhdlc/ring_buffer.c
    libhdlc/control_frame.c
    libhdlc/spsc_ring.c)
target_include_directories(hdlc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hdlc PRIVATE house_warnings)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(hdlc PUBLIC Threads::Threads)
endif()
add_library(hdlc::hdlc ALIAS hdlc)

if(HDLC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(../extras/tests ${CMAKE_CURRENT_BINARY_DIR}/tests)
endif()
