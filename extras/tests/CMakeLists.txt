# Unit tests: one executable per module, plain C, no framework (see test_util.h).
#
# xmsg_add_test(name) builds extras/tests/test_<name>.c against the library with the house
# warnings, and registers it with CTest. The PASS_REGULAR_EXPRESSION makes a test that dies before
# its verdict line count as a failure even when the exit code is lost.
#
# Integration tests (label "integration") talk to a running nd100x and SKIP (exit code 2) when the
# environment variable ND100X_HDLC_HOST is not set, so a plain `ctest` is always runnable.

# The peer-to-peer test transport (in-memory pair with faults, loopback TCP). Test code, so it
# may use the C library freely; it still builds with the house warnings.
add_library(xmsg_testpair STATIC pair/test_pair.c)
target_include_directories(xmsg_testpair PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xmsg_testpair PUBLIC xmsg::xmsg PRIVATE house_warnings)
if(WIN32)
    target_link_libraries(xmsg_testpair PUBLIC ws2_32)
endif()

function(xmsg_add_test name)
    add_executable(test_${name} test_${name}.c)
    target_link_libraries(test_${name} PRIVATE xmsg::xmsg xmsg_testpair house_warnings)
    target_include_directories(test_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND test_${name})
    set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "TB_RESULT: PASS")
endfunction()

function(xmsg_add_integration_test name)
    add_executable(test_${name} integration/test_${name}.c)
    target_link_libraries(test_${name} PRIVATE xmsg::xmsg xmsg_testpair house_warnings)
    target_include_directories(test_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND test_${name})
    set_tests_properties(${name} PROPERTIES
        PASS_REGULAR_EXPRESSION "TB_RESULT: PASS"
        SKIP_RETURN_CODE 2
        LABELS integration)
endfunction()

xmsg_add_test(smoke)
xmsg_add_test(log)
xmsg_add_test(pair_hdlc)
xmsg_add_test(lapb)
xmsg_add_test(pair_lapb)
xmsg_add_test(sintran_hdr)
xmsg_add_test(xmsg_dg)
xmsg_add_test(xmsg_flags1)
xmsg_add_test(xmsg_frag)
xmsg_add_test(pair_node)
xmsg_add_test(xrout_route)
xmsg_add_test(pair_route)
xmsg_add_test(tad_asker)
xmsg_add_test(tad_responder)
xmsg_add_test(term)
xmsg_add_test(auth)
xmsg_add_test(console)
xmsg_add_test(console_tad)
xmsg_add_integration_test(nd100x_lapb)
xmsg_add_integration_test(nd100x_tad)
