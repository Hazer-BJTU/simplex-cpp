add_executable(test_editing_tools test_tools.cpp)
target_link_libraries(test_editing_tools PRIVATE
    tools_intrinsic_editing Boost::unit_test_framework)
add_test(NAME test_editing_tools COMMAND test_editing_tools)
set_tests_properties(test_editing_tools PROPERTIES TIMEOUT 30)
