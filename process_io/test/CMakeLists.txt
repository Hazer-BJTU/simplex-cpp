add_executable(process_io_child child.cpp)

add_executable(test_process_io_capture test_capture.cpp)
target_link_libraries(test_process_io_capture PRIVATE process_io_lib Boost::unit_test_framework)
add_test(NAME process_io_capture COMMAND test_process_io_capture)

add_executable(test_process_io_pipe test_pipe.cpp)
target_link_libraries(test_process_io_pipe PRIVATE process_io_lib Boost::unit_test_framework)
target_compile_definitions(test_process_io_pipe PRIVATE
    PROCESS_IO_CHILD="$<TARGET_FILE:process_io_child>")
add_dependencies(test_process_io_pipe process_io_child)
add_test(NAME process_io_pipe COMMAND test_process_io_pipe)
set_tests_properties(process_io_pipe PROPERTIES TIMEOUT 30)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_process_io_failures test_failures.cpp)
    target_link_libraries(test_process_io_failures PRIVATE
        process_io_lib Boost::unit_test_framework ${CMAKE_DL_LIBS})
    target_compile_definitions(test_process_io_failures PRIVATE
        PROCESS_IO_CHILD="$<TARGET_FILE:process_io_child>")
    target_link_options(test_process_io_failures PRIVATE
        "LINKER:--export-dynamic-symbol=read"
        "LINKER:--export-dynamic-symbol=write")
    add_dependencies(test_process_io_failures process_io_child)
    add_test(NAME process_io_failures COMMAND test_process_io_failures)
    set_tests_properties(process_io_failures PROPERTIES TIMEOUT 15)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(process_pty_child terminal_child.cpp)
    add_executable(test_process_io_pty test_pty.cpp)
    target_link_libraries(test_process_io_pty PRIVATE process_io_lib Boost::unit_test_framework)
    target_compile_definitions(test_process_io_pty PRIVATE
        PROCESS_IO_CHILD="$<TARGET_FILE:process_io_child>"
        PROCESS_PTY_CHILD="$<TARGET_FILE:process_pty_child>")
    add_dependencies(test_process_io_pty process_io_child process_pty_child)
    add_test(NAME process_io_pty COMMAND test_process_io_pty)
    set_tests_properties(process_io_pty PROPERTIES TIMEOUT 45)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_process_io_pty_failures test_pty_failures.cpp)
    target_link_libraries(test_process_io_pty_failures PRIVATE
        process_io_lib Boost::unit_test_framework ${CMAKE_DL_LIBS})
    target_compile_definitions(test_process_io_pty_failures PRIVATE
        PROCESS_IO_CHILD="$<TARGET_FILE:process_io_child>"
        PROCESS_PTY_CHILD="$<TARGET_FILE:process_pty_child>")
    target_link_options(test_process_io_pty_failures PRIVATE
        "LINKER:--export-dynamic-symbol=posix_openpt"
        "LINKER:--export-dynamic-symbol=grantpt"
        "LINKER:--export-dynamic-symbol=setsid"
        "LINKER:--export-dynamic-symbol=dup2"
        "LINKER:--export-dynamic-symbol=read"
        "LINKER:--export-dynamic-symbol=write")
    add_dependencies(test_process_io_pty_failures process_io_child process_pty_child)
    add_test(NAME process_io_pty_failures COMMAND test_process_io_pty_failures)
    set_tests_properties(process_io_pty_failures PROPERTIES TIMEOUT 30)
endif()
