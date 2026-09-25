foreach(name configuration confirmation application protocol)
    add_executable(test_core_${name} test_${name}.cpp)
    target_link_libraries(test_core_${name} PRIVATE core_lib Boost::unit_test_framework)
    add_test(NAME test_core_${name} COMMAND test_core_${name})
    set_tests_properties(test_core_${name} PROPERTIES TIMEOUT 30)
endforeach()
target_link_libraries(test_core_protocol PRIVATE llm_chat_completions)

add_test(NAME core_worker_container
    COMMAND /bin/sh ${PROJECT_SOURCE_DIR}/loop/test/run_example_container_smoke.sh
        ${CMAKE_CURRENT_SOURCE_DIR}/test_worker_container.py
        $<TARGET_FILE:simplex_worker> $<TARGET_FILE:simplex_shell>)
set_tests_properties(core_worker_container PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

add_test(NAME core_shell_protocol
    COMMAND /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/run_python_test.sh
        ${CMAKE_CURRENT_SOURCE_DIR}/test_shell.py $<TARGET_FILE:simplex_shell>)
set_tests_properties(core_shell_protocol PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Export the test executable's resolver override for interposition in Asio.
    # Keeping the fixture in this executable avoids a test-only DT_NEEDED
    # dependency and removes any need for LD_PRELOAD in staged runtime tests.
    add_executable(test_core_dns test_dns.cpp slow_resolver.cpp)
    target_link_options(test_core_dns PRIVATE "LINKER:--export-dynamic-symbol=getaddrinfo")
    target_link_libraries(test_core_dns PRIVATE core_lib ${CMAKE_DL_LIBS} Boost::unit_test_framework)
    add_test(NAME test_core_dns COMMAND test_core_dns)
    set_tests_properties(test_core_dns PROPERTIES TIMEOUT 15)
endif()

add_test(NAME core_lifecycle_container
    COMMAND /bin/sh ${PROJECT_SOURCE_DIR}/loop/test/run_example_container_smoke.sh
        ${CMAKE_CURRENT_SOURCE_DIR}/test_lifecycle_container.py $<TARGET_FILE:simplex_worker>)
set_tests_properties(core_lifecycle_container PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

add_test(NAME core_worker_cli
    COMMAND /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/run_python_test.sh
        ${CMAKE_CURRENT_SOURCE_DIR}/test_worker_cli.py $<TARGET_FILE:simplex_worker>)
set_tests_properties(core_worker_cli PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
