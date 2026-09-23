add_executable(test_loop_extensions test_plugin.cpp)
target_link_libraries(test_loop_extensions PRIVATE
    loop_extensions Boost::unit_test_framework)
add_dependencies(test_loop_extensions loop_extension_noop)
add_test(NAME test_loop_extensions COMMAND test_loop_extensions)

foreach(kind bad_abi wrong_context missing_factory no_magic
        null_product throw_product nonstandard_product wrong_product)
    add_library(loop_fixture_${kind} MODULE fixture.cpp)
    target_link_libraries(loop_fixture_${kind} PRIVATE loop_extensions boost_dll_iface)
    set_target_properties(loop_fixture_${kind} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/${kind}")
endforeach()
target_compile_definitions(loop_fixture_bad_abi PRIVATE SIMPLEX_TEST_BAD_ABI)
target_compile_definitions(loop_fixture_wrong_context PRIVATE SIMPLEX_TEST_WRONG_CONTEXT)
target_compile_definitions(loop_fixture_missing_factory PRIVATE SIMPLEX_TEST_MISSING_FACTORY)
target_compile_definitions(loop_fixture_no_magic PRIVATE SIMPLEX_TEST_NO_MAGIC)
add_dependencies(test_loop_extensions
    loop_fixture_bad_abi loop_fixture_wrong_context
    loop_fixture_missing_factory loop_fixture_no_magic)
target_compile_definitions(test_loop_extensions PRIVATE
    LOOP_FIXTURE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/fixtures")

target_compile_definitions(loop_fixture_null_product PRIVATE SIMPLEX_TEST_NULL_PRODUCT)
add_dependencies(test_loop_extensions loop_fixture_null_product)

target_compile_definitions(loop_fixture_throw_product PRIVATE SIMPLEX_TEST_THROW_PRODUCT)
add_dependencies(test_loop_extensions loop_fixture_throw_product)

target_compile_definitions(loop_fixture_nonstandard_product PRIVATE SIMPLEX_TEST_NONSTANDARD_PRODUCT)
add_dependencies(test_loop_extensions loop_fixture_nonstandard_product)

add_dependencies(test_loop_extensions loop_fixture_wrong_product)
