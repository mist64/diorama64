function(sm64ar_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE ar_core ar_platform_fake ar_cube_adapter ar_render_common ar_vos_logic ar_qx_convert)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
endfunction()

sm64ar_test(test_runtime_cube test_runtime_cube.cpp)
sm64ar_test(test_qx_convert test_qx_convert.cpp)
