add_executable(test_atomic_write test_atomic_write.cpp)
target_link_libraries(test_atomic_write PRIVATE fileio_lib Boost::unit_test_framework)
add_test(NAME test_atomic_write COMMAND test_atomic_write)

add_executable(test_base64 test_base64.cpp)
target_link_libraries(test_base64 PRIVATE fileio_lib Boost::unit_test_framework OpenSSL::Crypto)
target_compile_definitions(test_base64 PRIVATE
    TEST_BASE64_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_test(NAME test_base64 COMMAND test_base64)

# Lexical path validation, ancestor symlink refusal and shared session leases.
add_executable(test_fileio_paths test_fileio_paths.cpp)
target_link_libraries(test_fileio_paths PRIVATE fileio_lib Boost::unit_test_framework)
add_test(NAME test_fileio_paths COMMAND test_fileio_paths)

# Fixed-buffer sequential scan used by textedit's streaming reads and edits.
add_executable(test_fileio_streaming test_streaming.cpp)
target_link_libraries(test_fileio_streaming PRIVATE fileio_lib Boost::unit_test_framework)
add_test(NAME test_fileio_streaming COMMAND test_fileio_streaming)

# Replacement transaction: lazy candidate, prefix copy, observer and rewind.
add_executable(test_staged_replace test_staged_replace.cpp)
target_link_libraries(test_staged_replace PRIVATE fileio_lib Boost::unit_test_framework)
add_test(NAME test_staged_replace COMMAND test_staged_replace)
