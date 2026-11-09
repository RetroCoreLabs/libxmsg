# Unit tests: one executable per module, plain C, no framework (see test_util.h, the same harness
# as libxmsg). CTest looks for "TB_RESULT: PASS" so a test that dies early counts as failed.
function(hdlc_add_test name)
    add_executable(test_${name} test_${name}.c)
    target_link_libraries(test_${name} PRIVATE hdlc::hdlc house_warnings)
    target_include_directories(test_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND test_${name})
    set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "TB_RESULT: PASS")
endfunction()

hdlc_add_test(hdlc)
hdlc_add_test(byte_frame)
hdlc_add_test(ring_buffer)
hdlc_add_test(control_frame)
hdlc_add_test(spsc_ring)
