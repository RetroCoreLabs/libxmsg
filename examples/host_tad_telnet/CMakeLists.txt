# host_tad_telnet - telnet clients to TAD sessions on a SINTRAN machine (Windows, Linux, macOS).
#
# Built from src/CMakeLists.txt when XMSG_BUILD_EXAMPLES is on. A host program: it may print,
# allocate and use sockets.

add_executable(host_tad_telnet host_tad_telnet.c)
target_link_libraries(host_tad_telnet PRIVATE xmsg::xmsg house_warnings)
if(WIN32)
    target_link_libraries(host_tad_telnet PRIVATE ws2_32)
endif()
