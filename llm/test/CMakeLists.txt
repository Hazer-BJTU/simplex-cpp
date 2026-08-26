# Unit tests for the llm model-plugin contract (llm/models.hpp).
#
# test_llm_models — in-memory coverage of the model-type vocabulary, the
# default converse() throw, the integrate() placement table (incl. orphan
# folds and the persistence guarantee), and the dispatcher path over in-memory
# contexts (add + create_model routing, the build/release lifecycle guarantee
# incl. build failure, the ABI/type/warm admission gates). The real dynamic
# path (a provider .so under bin/plugins/llm, load_models over it) and the
# offline loopback full-flow exchange are covered by test_openai_plugin and
# the responses/* tests below.
#
# llm_iface already carries Boost headers (Asio), nlohmann/json,
# extension_framework, and the generated versioning header.

add_executable(test_llm_models test_models.cpp)
target_compile_options(test_llm_models PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
target_link_libraries(test_llm_models PRIVATE llm_iface Boost::unit_test_framework)
add_test(NAME test_llm_models COMMAND test_llm_models)

add_executable(test_responses_interpreter
    responses/test_responses_interpreter.cpp)
target_link_libraries(test_responses_interpreter PRIVATE
    llm_responses Boost::unit_test_framework)
add_test(NAME test_responses_interpreter COMMAND test_responses_interpreter)

add_executable(test_responses_stream responses/test_responses_stream.cpp)
target_link_libraries(test_responses_stream PRIVATE
    llm_responses Boost::unit_test_framework)
add_test(NAME test_responses_stream COMMAND test_responses_stream)

add_executable(test_responses_reader responses/test_responses_reader.cpp)
target_include_directories(test_responses_reader PRIVATE
    "${PROJECT_SOURCE_DIR}/endpoint/test")
target_link_libraries(test_responses_reader PRIVATE
    llm_responses endpoint_lib Boost::unit_test_framework)
add_test(NAME test_responses_reader COMMAND test_responses_reader)

add_executable(test_responses_model responses/test_responses_model.cpp)
target_include_directories(test_responses_model PRIVATE
    "${PROJECT_SOURCE_DIR}/endpoint/test")
target_link_libraries(test_responses_model PRIVATE
    llm_responses endpoint_lib Boost::unit_test_framework)
add_test(NAME test_responses_model COMMAND test_responses_model)

add_executable(test_chat_completions_interpreter
    chat_completions/test_chat_completions_interpreter.cpp)
target_link_libraries(test_chat_completions_interpreter PRIVATE
    llm_chat_completions Boost::unit_test_framework)
add_test(NAME test_chat_completions_interpreter
    COMMAND test_chat_completions_interpreter)

add_executable(test_chat_completions_stream
    chat_completions/test_chat_completions_stream.cpp)
target_link_libraries(test_chat_completions_stream PRIVATE
    llm_chat_completions Boost::unit_test_framework)
add_test(NAME test_chat_completions_stream
    COMMAND test_chat_completions_stream)

add_executable(test_openai_plugin test_openai_plugin.cpp)
target_link_libraries(test_openai_plugin PRIVATE
    llm_iface Boost::unit_test_framework)
target_compile_definitions(test_openai_plugin PRIVATE
    OPENAI_PLUGIN_DIR="$<TARGET_FILE_DIR:llm_openai>")
add_dependencies(test_openai_plugin llm_openai)
add_test(NAME test_openai_plugin COMMAND test_openai_plugin)
