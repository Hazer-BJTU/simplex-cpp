# Unit tests for the llm model-plugin contract (llm/models.hpp) and for the
# plugin/ABI boundary itself.
#
# The two protocol adapters have their own suites next to their code:
#   compat/chat_completions/test/  — interpreter, stream, reader, model, tool
#                                    correlation, concurrency
#   compat/responses/test/         — interpreter, stream, reader, model
# What is left here is what belongs to the MODULE rather than to one adapter.
#
# test_llm_models — in-memory coverage of the model-type vocabulary, the
# default converse() throw, the integrate() placement table (incl. orphan
# folds and the persistence guarantee), and the dispatcher path over in-memory
# contexts (add + create_model routing, the build/release lifecycle guarantee
# incl. build failure, the ABI/type/warm admission gates). The real dynamic
# path (a provider .so under bin/plugins/llm, load_models over it) and the
# offline loopback full-flow exchange are covered by test_openai_plugin and the
# adapter suites above.
#
# llm_iface already carries Boost headers (Asio), nlohmann/json,
# extensions, and the generated versioning header.

add_executable(test_llm_models test_models.cpp)
target_compile_options(test_llm_models PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
target_link_libraries(test_llm_models PRIVATE llm_iface Boost::unit_test_framework)
add_test(NAME test_llm_models COMMAND test_llm_models)

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
#
# llm_chat_completions is named explicitly because this test includes the
# adapter's events.hpp — the reasoning increment a host subscribes to. That
# header no longer arrives through llm_iface's include directory now that the
# adapters live in the compatibility layer, and a consumer of a layer's header
# naming that layer is the point of the move rather than a cost of it.
add_executable(test_deepseek_plugin test_deepseek_plugin.cpp)
target_include_directories(test_deepseek_plugin PRIVATE
    "${PROJECT_SOURCE_DIR}/endpoint/test")
target_link_libraries(test_deepseek_plugin PRIVATE
    llm_iface llm_chat_completions endpoint_lib eventbus_iface
    Boost::unit_test_framework)
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
#
# It stays at the module level rather than moving into compat/ because what it
# asserts is the relationship between THREE parties — the host executable, the
# provider plugins and the two shared adapters — and neither adapter owns it
# alone.
add_test(NAME llm_plugin_boundary_hygiene COMMAND ${CMAKE_COMMAND}
    -DDEEPSEEK_SO=$<TARGET_FILE:llm_deepseek>
    -DOPENAI_SO=$<TARGET_FILE:llm_openai>
    -DHOST_EXE=$<TARGET_FILE:test_deepseek_plugin>
    -DCHAT_LIB=$<TARGET_FILE:llm_chat_completions>
    -DRESPONSES_LIB=$<TARGET_FILE:llm_responses>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/plugin_boundary_hygiene.cmake)
