# libxmsg - SINTRAN III XMSG over LAPB over HDLC, portable C11.
#
# Consumers:   add_subdirectory(path/to/libxmsg/src)   then   target_link_libraries(app PRIVATE xmsg::xmsg)
#              (hdlc::hdlc alone: add -DLIBXMSG_WITH_XMSG=OFF)
# Developers:  cmake -S src -B build -DXMSG_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build
#
# Only the libraries, the tests and the host examples are built from here. The Pico examples are
# their own top-level CMake projects (they need pico_sdk_init() before project()), and the Arduino
# sketches are compiled by the Arduino IDE, which reads library.properties at the repository root.
cmake_minimum_required(VERSION 3.13)
project(libxmsg VERSION 0.1.0 LANGUAGES C)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(XMSG_IS_TOP_LEVEL ON)
else()
    set(XMSG_IS_TOP_LEVEL OFF)
endif()

option(XMSG_WITH_COSMOS "Build libcosmos (COSMOS file access client and server)" OFF)
option(XMSG_WITH_FTP "Build the FTP command core inside libcosmos (needs XMSG_WITH_COSMOS)" OFF)
option(XMSG_BUILD_TESTS "Build the CTest unit tests" ${XMSG_IS_TOP_LEVEL})
option(XMSG_BUILD_EXAMPLES "Build the host examples" ${XMSG_IS_TOP_LEVEL})
option(XMSG_WERROR "Treat every warning as an error (CI only)" OFF)

set(XMSG_MAX_SESSIONS 20 CACHE STRING "Sessions (TAD, FA, ...) one node can hold; host_tad_telnet needs 16, XMSGGW uses 20")
set(XMSG_MAX_PEERS 4 CACHE STRING "Remote nodes with their own outgoing message counter")
set(XMSG_REASSEMBLY_SLOTS 1 CACHE STRING "Fragmented messages held while their second half arrives")
set(XMSG_LAPB_N1 622 CACHE STRING "Largest LAPB information field in bytes")
set(XMSG_LOG_MAX_LEVEL 4 CACHE STRING "Highest log level compiled in: 0 ERROR 1 WARN 2 INFO 3 DEBUG 4 TRACE")

include(cmake/house_standard.cmake)

# ---------------------------------------------------------------------------------------------
# libhdlc: ALL HDLC code (bit-level framing, CRC-16, RFC 1662 byte framing, ring buffers) lives
# in src/libhdlc (Ronny, 2026-11-09; moved into this repository from RetroCoreLabs/libhdlc on
# 2026-11-10). hdlc::hdlc is its own target and can be used without libxmsg: set
# LIBXMSG_WITH_XMSG=OFF to build only it (RC422Dev on the Pico). A parent project that already
# added an hdlc::hdlc keeps its own.
# ---------------------------------------------------------------------------------------------
option(LIBXMSG_WITH_XMSG "Build libxmsg as well as libhdlc (OFF: libhdlc alone)" ON)
if(NOT TARGET hdlc::hdlc)
    add_library(hdlc STATIC
        libhdlc/hdlc.c
        libhdlc/byte_frame.c
        libhdlc/ring_buffer.c
        libhdlc/control_frame.c
        libhdlc/spsc_ring.c)
    target_include_directories(hdlc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(hdlc PRIVATE house_warnings)
    if(PICO_SDK_VERSION_STRING)
        # Raspberry Pi Pico SDK build. pico_platform_headers carries PICO_BUILD / PICO_ON_DEVICE
        # (so ring_buffer.c compiles to nothing and HOT_PATH_CODE becomes __not_in_flash) and the
        # header that defines __not_in_flash. Without it both would silently take the host path.
        # The header-only target, not pico_platform: that one adds the SDK's platform.c to this
        # library, where the house warnings then flag the SDK's own code (-Wundef, SDK 1.5.1).
        # No pthreads here: the Pico uses spsc_ring.
        if(NOT TARGET pico_platform_headers)
            message(FATAL_ERROR "libhdlc: Pico SDK ${PICO_SDK_VERSION_STRING} has no pico_platform_headers target; "
                                "libhdlc needs it for PICO_ON_DEVICE and __not_in_flash.")
        endif()
        target_link_libraries(hdlc PUBLIC pico_platform_headers)
    elseif(NOT WIN32)
        # Linux and other POSIX hosts: ring_buffer.c locks with a pthread mutex.
        find_package(Threads REQUIRED)
        target_link_libraries(hdlc PUBLIC Threads::Threads)
    endif()
    add_library(hdlc::hdlc ALIAS hdlc)
    if(XMSG_BUILD_TESTS)
        enable_testing()
        add_subdirectory(../extras/tests/libhdlc ${CMAKE_CURRENT_BINARY_DIR}/tests/libhdlc)
    endif()
endif()
if(NOT LIBXMSG_WITH_XMSG)
    return()
endif()

# ---------------------------------------------------------------------------------------------
# libxmsg
# ---------------------------------------------------------------------------------------------
# Header-only until P1 lands the first .c file; the empty source list is deliberate so the target
# and its usage requirements exist from P0 on. CMake needs at least one source for a STATIC library,
# so a one-line translation unit is generated here until then.
set(XMSG_SOURCES
    libxmsg/log/xmsg_log.c
    libxmsg/lapb/lapb.c
    libxmsg/sintran/sintran_hdr.c
    libxmsg/xmsg/xmsg_subhdr.c
    libxmsg/xmsg/xmsg_dg.c
    libxmsg/xmsg/xmsg_flags1.c
    libxmsg/xmsg/xmsg_portword.c
    libxmsg/xmsg/xmsg_frag.c
    libxmsg/xmsg/xmsg_node.c
    libxmsg/xrout/xrout_letter.c
    libxmsg/xrout/xrout_route.c
    libxmsg/xrout/xrout_walk.c
    libxmsg/xrout/xrout_name.c
    libxmsg/xrout/xrout_names_walk.c
    libxmsg/tad/tad_codec.c
    libxmsg/tad/tad_asker.c
    libxmsg/tad/tad_responder.c
    libxmsg/term/term_escape.c
    libxmsg/term/term_keyseq.c
    libxmsg/term/term_line.c
    libxmsg/term/term_report.c
    libxmsg/term/term_telnet.c
    libxmsg/auth/auth_hash.c
    libxmsg/auth/auth_user.c
    libxmsg/auth/auth_login.c
    libxmsg/console/console.c
    libxmsg/console/console_tad.c)

if(NOT XMSG_SOURCES)
    set(XMSG_PLACEHOLDER_C "${CMAKE_CURRENT_BINARY_DIR}/xmsg_placeholder.c")
    file(WRITE "${XMSG_PLACEHOLDER_C}"
        "/* Generated by CMake: libxmsg has no .c files yet. Removed when the first module lands. */\n"
        "typedef int xmsg_placeholder_type;\n")
    list(APPEND XMSG_SOURCES "${XMSG_PLACEHOLDER_C}")
endif()

add_library(xmsg STATIC ${XMSG_SOURCES})
target_include_directories(xmsg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(xmsg PUBLIC
    XMSG_MAX_SESSIONS=${XMSG_MAX_SESSIONS}
    XMSG_MAX_PEERS=${XMSG_MAX_PEERS}
    XMSG_REASSEMBLY_SLOTS=${XMSG_REASSEMBLY_SLOTS}
    XMSG_LAPB_N1=${XMSG_LAPB_N1}
    XMSG_LOG_MAX_LEVEL=${XMSG_LOG_MAX_LEVEL})
target_link_libraries(xmsg PUBLIC hdlc::hdlc PRIVATE house_warnings)
add_library(xmsg::xmsg ALIAS xmsg)

# ---------------------------------------------------------------------------------------------
# libcosmos (P6 onward)
# ---------------------------------------------------------------------------------------------
# COSMOS file access over XMSG: QFORM, the FA messages and the read client. Needs xmsg::xmsg.
if(XMSG_WITH_COSMOS)
    add_library(cosmos STATIC
        libcosmos/qform/qform.c
        libcosmos/sintran/sintran_password.c
        libcosmos/fa/fa_envelope.c
        libcosmos/fa/fa_requests.c
        libcosmos/fa/fa_replies.c
        libcosmos/fa/fa_client.c)
    target_include_directories(cosmos PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cosmos PUBLIC xmsg::xmsg PRIVATE house_warnings)
    add_library(cosmos::cosmos ALIAS cosmos)
endif()
if(XMSG_WITH_FTP AND NOT XMSG_WITH_COSMOS)
    message(FATAL_ERROR "XMSG_WITH_FTP needs XMSG_WITH_COSMOS.")
endif()

# ---------------------------------------------------------------------------------------------
# Tests and host examples
# ---------------------------------------------------------------------------------------------
if(XMSG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(../extras/tests ${CMAKE_CURRENT_BINARY_DIR}/tests)
endif()

if(XMSG_BUILD_EXAMPLES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../examples/host_tad_telnet/CMakeLists.txt)
    add_subdirectory(../examples/host_tad_telnet ${CMAKE_CURRENT_BINARY_DIR}/examples/host_tad_telnet)
endif()
