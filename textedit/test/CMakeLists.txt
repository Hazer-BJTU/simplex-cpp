add_executable(test_utf8_probe test_utf8_probe.cpp)
target_link_libraries(test_utf8_probe PRIVATE textedit_lib Boost::unit_test_framework)
add_test(NAME test_utf8_probe COMMAND test_utf8_probe)
set_tests_properties(test_utf8_probe PROPERTIES TIMEOUT 30)

add_executable(test_line_index test_line_index.cpp)
target_link_libraries(test_line_index PRIVATE textedit_lib Boost::unit_test_framework)
add_test(NAME test_line_index COMMAND test_line_index)
set_tests_properties(test_line_index PROPERTIES TIMEOUT 30)

add_executable(test_text_read test_read.cpp)
target_link_libraries(test_text_read PRIVATE textedit_lib Boost::unit_test_framework)
add_test(NAME test_text_read COMMAND test_text_read)
set_tests_properties(test_text_read PROPERTIES TIMEOUT 30)

add_executable(test_text_edit test_edit.cpp)
target_link_libraries(test_text_edit PRIVATE textedit_lib fileio_lib Boost::unit_test_framework)
add_test(NAME test_text_edit COMMAND test_text_edit)
set_tests_properties(test_text_edit PROPERTIES TIMEOUT 30)

add_executable(test_edit_publication test_edit_publication.cpp)
target_link_libraries(test_edit_publication PRIVATE textedit_lib Boost::unit_test_framework)
set_target_properties(test_edit_publication PROPERTIES ENABLE_EXPORTS ON)
add_test(NAME test_edit_publication COMMAND test_edit_publication)
set_tests_properties(test_edit_publication PROPERTIES TIMEOUT 30)

add_executable(test_edit_initial_conflict test_edit_initial_conflict.cpp)
target_link_libraries(test_edit_initial_conflict PRIVATE textedit_lib Boost::unit_test_framework)
set_target_properties(test_edit_initial_conflict PROPERTIES ENABLE_EXPORTS ON)
add_test(NAME test_edit_initial_conflict COMMAND test_edit_initial_conflict)
set_tests_properties(test_edit_initial_conflict PROPERTIES TIMEOUT 30)

add_executable(test_stream_read test_stream_read.cpp)
target_link_libraries(test_stream_read PRIVATE textedit_lib Boost::unit_test_framework)
# The sink retention invariant is a white-box check over an internal header.
target_include_directories(test_stream_read PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
# Executable symbol interposition caps read() so every buffer edge is exercised.
set_target_properties(test_stream_read PROPERTIES ENABLE_EXPORTS ON)
add_test(NAME test_stream_read COMMAND test_stream_read)
set_tests_properties(test_stream_read PROPERTIES TIMEOUT 120)

add_executable(test_stream_edit test_stream_edit.cpp)
target_link_libraries(test_stream_edit PRIVATE textedit_lib fileio_lib Boost::unit_test_framework)
# Executable symbol interposition caps read() so every buffer edge is exercised.
set_target_properties(test_stream_edit PROPERTIES ENABLE_EXPORTS ON)
add_test(NAME test_stream_edit COMMAND test_stream_edit)
set_tests_properties(test_stream_edit PROPERTIES TIMEOUT 180)

# A small pthread stack catches fixed-buffer overflow on glibc as well as musl.
add_executable(test_edit_small_stack test_edit_small_stack.cpp)
target_link_libraries(test_edit_small_stack PRIVATE textedit_lib Boost::unit_test_framework)
add_test(NAME test_edit_small_stack COMMAND test_edit_small_stack)
set_tests_properties(test_edit_small_stack PROPERTIES TIMEOUT 30)
