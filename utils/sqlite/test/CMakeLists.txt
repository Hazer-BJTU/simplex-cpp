add_executable(test_sqlite test_database.cpp)
target_link_libraries(test_sqlite PRIVATE sqlite_lib sqlite_runtime Boost::unit_test_framework Boost::filesystem)
add_test(NAME test_sqlite COMMAND test_sqlite)
set_tests_properties(test_sqlite PROPERTIES TIMEOUT 30)
