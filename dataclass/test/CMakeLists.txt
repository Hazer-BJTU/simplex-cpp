# Round-trip tests for the model_io records' nlohmann ADL to_json/from_json.
# Pure data
# checks — no network, no filesystem. Boost.Test supplies the executable's test
# runner.
add_executable(test_model_io test_model_io.cpp)
target_link_libraries(test_model_io PRIVATE
    dataclass_iface
    Boost::unit_test_framework)
add_test(NAME dataclass_model_io COMMAND test_model_io)

# PromptTemplate assembly/stability tests: markdown rendering, tier
# enforcement, and the byte-prefix cache guarantees. Same pure-data shape.
add_executable(test_prompt_template test_prompt_template.cpp)
target_link_libraries(test_prompt_template PRIVATE
    dataclass_iface
    Boost::unit_test_framework)
add_test(NAME dataclass_prompt_template COMMAND test_prompt_template)
