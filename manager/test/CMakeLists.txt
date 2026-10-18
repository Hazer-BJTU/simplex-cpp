add_executable(test_manager test_manager.cpp)
target_link_libraries(test_manager PRIVATE manager_lib load_persistence persistence_lib Boost::unit_test_framework)
add_test(NAME test_manager COMMAND test_manager)
set_tests_properties(test_manager PROPERTIES TIMEOUT 30)
add_test(NAME manager_cli COMMAND /bin/sh
    ${PROJECT_SOURCE_DIR}/core/test/run_python_test.sh
    ${CMAKE_CURRENT_SOURCE_DIR}/test_cli.py $<TARGET_FILE:simplex_manager>
    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/simplex)
set_tests_properties(manager_cli PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(manager_rename_pause SHARED pause_rename.c)
    target_link_libraries(manager_rename_pause PRIVATE ${CMAKE_DL_LIBS})
    add_test(NAME manager_concurrent COMMAND /bin/sh
        ${PROJECT_SOURCE_DIR}/core/test/run_python_test.sh
        ${CMAKE_CURRENT_SOURCE_DIR}/test_concurrent.py
        $<TARGET_FILE:simplex_manager> $<TARGET_FILE:manager_rename_pause>)
    set_tests_properties(manager_concurrent PROPERTIES TIMEOUT 40 SKIP_RETURN_CODE 77)
endif()
