add_executable(test_executable_location location.cpp)
target_link_libraries(test_executable_location PRIVATE executable_iface)
add_test(NAME test_executable_location
    COMMAND test_executable_location $<TARGET_FILE:test_executable_location>)
