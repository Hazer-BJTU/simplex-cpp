add_executable(test_atomic_write test_atomic_write.cpp)
target_link_libraries(test_atomic_write PRIVATE fileio_lib Boost::unit_test_framework)
add_test(NAME test_atomic_write COMMAND test_atomic_write)

add_executable(test_base64 test_base64.cpp)
target_link_libraries(test_base64 PRIVATE fileio_lib Boost::unit_test_framework OpenSSL::Crypto)
target_compile_definitions(test_base64 PRIVATE
    TEST_BASE64_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_test(NAME test_base64 COMMAND test_base64)
