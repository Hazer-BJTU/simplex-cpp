file(REMOVE_RECURSE "${stage}")
file(MAKE_DIRECTORY "${stage}/bin/schemas/loop/context_statistic")
configure_file("${executable}" "${stage}/bin/test_installed_config" COPYONLY)
file(READ "${source_config}" config)
string(REPLACE "context_window_tokens: 128000"
               "context_window_tokens: 777" staged_config "${config}")
if(staged_config STREQUAL config)
    message(FATAL_ERROR "Source config did not contain the expected window")
endif()
file(WRITE "${stage}/bin/schemas/loop/context_statistic/config.yaml"
     "${staged_config}")
unset(ENV{SIMPLEX_LOOP_HOOK_SCHEMA_DIR})
execute_process(
    COMMAND "${stage}/bin/test_installed_config"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Staged executable did not load its modified config: ${result}")
endif()
