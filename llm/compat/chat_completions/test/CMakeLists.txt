# Tests for the Chat Completions adapter, next to the code they cover (the
# layer's own CMakeLists adds this directory when BUILD_TESTS is on).
#
# They come in two kinds: the unit-level ones over the interpreter, the stream
# handler and the reader (request construction and event assembly against
# hand-written frames), and the model-level ones that run a whole exchange over
# the endpoint module's loopback transport. endpoint/test is on the include path
# for loopback_server.hpp, exactly as in the responses suite.
#
# llm_chat_completions carries llm_iface (the contract), endpoint_lib (the
# transport the loopback fixture speaks) and eventbus_iface (the reasoning
# increments a host subscribes to), so the tests below only add Boost.Test.

add_executable(test_chat_completions_interpreter
    test_chat_completions_interpreter.cpp)
target_link_libraries(test_chat_completions_interpreter PRIVATE
    llm_chat_completions Boost::unit_test_framework)
add_test(NAME test_chat_completions_interpreter
    COMMAND test_chat_completions_interpreter)

# The tool layer's records correlated all the way to the wire: a real
# ToolRegistry runs a real batch whose failure carries a NESTED call id, the
# records become the turn's tool results, and the assertion is on the
# tool_call_id emit_tool_results() builds — the model's own call, never the
# nested one. Links tools_iface for the frame; the llm module itself does not
# depend on the tools module.
add_executable(test_tool_result_correlation test_tool_result_correlation.cpp)
target_compile_options(test_tool_result_correlation PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
target_link_libraries(test_tool_result_correlation PRIVATE
    llm_chat_completions tools_iface Boost::unit_test_framework)
add_test(NAME test_tool_result_correlation
    COMMAND test_tool_result_correlation)

add_executable(test_chat_completions_stream test_chat_completions_stream.cpp)
target_link_libraries(test_chat_completions_stream PRIVATE
    llm_chat_completions Boost::unit_test_framework)
add_test(NAME test_chat_completions_stream
    COMMAND test_chat_completions_stream)

# The chat-completions adapter's LLMModel surface: set_generation() layering
# on the build() derivation (and its survival across the idempotent second
# build), the GenerationPreset envelope folded into reasoning_effort by the
# interpreter, and provider_info() over the loopback transport (catalogue
# envelope unwrapping, array passthrough, non-200 and bad-shape failures).
add_executable(test_chat_completions_model test_chat_completions_model.cpp)
target_include_directories(test_chat_completions_model PRIVATE
    "${PROJECT_SOURCE_DIR}/endpoint/test")
target_link_libraries(test_chat_completions_model PRIVATE
    llm_chat_completions endpoint_lib Boost::unit_test_framework)
add_test(NAME test_chat_completions_model COMMAND test_chat_completions_model)

# The reentrancy contract of converse(): concurrent exchanges on ONE instance
# over a multi-threaded executor (answer-to-request pairing, exchange_id as the
# demultiplexing join key), a concurrent set_generation() that must not tear an
# in-flight snapshot, the by-value conversation argument surviving the
# temporary-into-co_spawn trap, and the retry replay's attempt counter.
# -pthread: the fixture holds one thread per concurrent connection and the test
# runs the io_context on a pool (the plain compile option, matching
# utils/eventbus/test and tools/test — it preserves the project's per-config
# option sets).
add_executable(test_chat_completions_concurrency
    test_chat_completions_concurrency.cpp)
target_include_directories(test_chat_completions_concurrency PRIVATE
    "${PROJECT_SOURCE_DIR}/endpoint/test")
target_compile_options(test_chat_completions_concurrency PRIVATE -pthread
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
target_link_libraries(test_chat_completions_concurrency PRIVATE
    llm_chat_completions endpoint_lib eventbus_iface
    Boost::unit_test_framework)
add_test(NAME test_chat_completions_concurrency
    COMMAND test_chat_completions_concurrency)
