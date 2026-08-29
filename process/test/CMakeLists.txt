# Contract tests for the process exception types: context retention, the
# what()/to_string() equivalence, and render omission of absent fields.
add_executable(test_exceptions test_exceptions.cpp)
target_link_libraries(test_exceptions PRIVATE
    process_iface
    Boost::unit_test_framework)
add_test(NAME process_exceptions COMMAND test_exceptions)
