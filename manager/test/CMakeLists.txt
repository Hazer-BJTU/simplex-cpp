add_executable(test_manager test_manager.cpp)
target_link_libraries(test_manager PRIVATE manager_lib load_persistence Boost::unit_test_framework)
add_test(NAME test_manager COMMAND test_manager)
set_tests_properties(test_manager PROPERTIES TIMEOUT 30)
add_test(NAME manager_cli COMMAND /bin/sh
    ${PROJECT_SOURCE_DIR}/core/test/run_python_test.sh
    ${CMAKE_CURRENT_SOURCE_DIR}/test_cli.py $<TARGET_FILE:simplex_manager>
    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/simplex)
set_tests_properties(manager_cli PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
