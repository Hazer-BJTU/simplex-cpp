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

# The chat-completions adapter's LLMModel surface: set_generation() layering
# on the build() derivation (and its survival across the idempotent second
# build), the GenerationPreset envelope folded into reasoning_effort by the
# interpreter, and provider_info() over the loopback transport (catalogue
# envelope unwrapping, array passthrough, non-200 and bad-shape failures).
add_executable(test_chat_completions_model
    chat_completions/test_chat_completions_model.cpp)
target_include_directories(test_chat_completions_model PRIVATE
    "${PROJECT_SOURCE_DIR}/endpoint/test")
target_link_libraries(test_chat_completions_model PRIVATE
    llm_chat_completions endpoint_lib Boost::unit_test_framework)
add_test(NAME test_chat_completions_model COMMAND test_chat_completions_model)

add_executable(test_openai_plugin test_openai_plugin.cpp)
target_link_libraries(test_openai_plugin PRIVATE
    llm_iface Boost::unit_test_framework)
target_compile_definitions(test_openai_plugin PRIVATE
    OPENAI_PLUGIN_DIR="$<TARGET_FILE_DIR:llm_openai>")
# ENABLE_EXPORTS (-rdynamic): the dlopened providers bind this executable's
# authoritative copies of the header-only contract classes' typeinfo/vtables
# (type identity across the boundary, not the name-comparison fallback).
set_target_properties(test_openai_plugin PROPERTIES ENABLE_EXPORTS ON)
# Both bundled provider .so share the plugins/llm directory the test counts;
# the dependency keeps a single-target build from racing a missing library.
add_dependencies(test_openai_plugin llm_openai llm_deepseek)
add_test(NAME test_openai_plugin COMMAND test_openai_plugin)

# The deepseek plugin test drives one exchange through the dlopened .so
# against a loopback server: endpoint_lib (transport + loopback fixture) and
# eventbus_iface (the host-side reasoning subscriber) are part of what the
# cross-boundary wiring test exercises.
add_executable(test_deepseek_plugin test_deepseek_plugin.cpp)
target_include_directories(test_deepseek_plugin PRIVATE
    "${PROJECT_SOURCE_DIR}/endpoint/test")
target_link_libraries(test_deepseek_plugin PRIVATE
    llm_iface endpoint_lib eventbus_iface Boost::unit_test_framework)
target_compile_definitions(test_deepseek_plugin PRIVATE
    DEEPSEEK_PLUGIN_DIR="$<TARGET_FILE_DIR:llm_deepseek>")
# ENABLE_EXPORTS (-rdynamic): the dlopened providers bind this executable's
# authoritative copies of the header-only contract classes' typeinfo/vtables
# (type identity across the boundary, not the name-comparison fallback).
set_target_properties(test_deepseek_plugin PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(test_deepseek_plugin llm_openai llm_deepseek)
add_test(NAME test_deepseek_plugin COMMAND test_deepseek_plugin)

# Structural assertions for the hardened plugin boundary: factory aliases
# exported, plugins bound to the shared adapter/asio runtimes by SONAME, the
# host exporting the contract typeinfo (-rdynamic), and the shared adapters
# owning the protocol classes' typeinfo. Pure nm/readelf over the built
# artifacts — see plugin_boundary_hygiene.cmake for the rationale of each
# check. The generator expressions reference targets test_deepseek_plugin
# already depends on, so a full build satisfies them.
add_test(NAME llm_plugin_boundary_hygiene COMMAND ${CMAKE_COMMAND}
    -DDEEPSEEK_SO=$<TARGET_FILE:llm_deepseek>
    -DOPENAI_SO=$<TARGET_FILE:llm_openai>
    -DHOST_EXE=$<TARGET_FILE:test_deepseek_plugin>
    -DCHAT_LIB=$<TARGET_FILE:llm_chat_completions>
    -DRESPONSES_LIB=$<TARGET_FILE:llm_responses>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/plugin_boundary_hygiene.cmake)
