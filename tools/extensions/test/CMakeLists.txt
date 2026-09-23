add_executable(test_tool_extensions test_plugin.cpp)
target_link_libraries(test_tool_extensions PRIVATE
    tools_extensions Boost::unit_test_framework)
add_dependencies(test_tool_extensions tools_extension_noop)
add_test(NAME test_tool_extensions COMMAND test_tool_extensions)

foreach(kind bad_abi wrong_context missing_factory no_magic
        null_product throw_product nonstandard_product wrong_product)
    add_library(tool_fixture_${kind} MODULE fixture.cpp)
    target_link_libraries(tool_fixture_${kind} PRIVATE tools_extensions boost_dll_iface)
    set_target_properties(tool_fixture_${kind} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/${kind}")
endforeach()
target_compile_definitions(tool_fixture_bad_abi PRIVATE SIMPLEX_TEST_BAD_ABI)
target_compile_definitions(tool_fixture_wrong_context PRIVATE SIMPLEX_TEST_WRONG_CONTEXT)
target_compile_definitions(tool_fixture_missing_factory PRIVATE SIMPLEX_TEST_MISSING_FACTORY)
target_compile_definitions(tool_fixture_no_magic PRIVATE SIMPLEX_TEST_NO_MAGIC)
add_dependencies(test_tool_extensions
    tool_fixture_bad_abi tool_fixture_wrong_context
    tool_fixture_missing_factory tool_fixture_no_magic)
target_compile_definitions(test_tool_extensions PRIVATE
    TOOL_FIXTURE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/fixtures")

target_compile_definitions(tool_fixture_null_product PRIVATE SIMPLEX_TEST_NULL_PRODUCT)
add_dependencies(test_tool_extensions tool_fixture_null_product)

target_compile_definitions(tool_fixture_throw_product PRIVATE SIMPLEX_TEST_THROW_PRODUCT)
add_dependencies(test_tool_extensions tool_fixture_throw_product)

target_compile_definitions(tool_fixture_nonstandard_product PRIVATE SIMPLEX_TEST_NONSTANDARD_PRODUCT)
add_dependencies(test_tool_extensions tool_fixture_nonstandard_product)

add_dependencies(test_tool_extensions tool_fixture_wrong_product)

add_library(tool_fixture_ownership MODULE ownership_fixture.cpp)
target_link_libraries(tool_fixture_ownership PRIVATE tools_extensions boost_dll_iface)
set_target_properties(tool_fixture_ownership PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/ownership")
add_dependencies(test_tool_extensions tool_fixture_ownership)
