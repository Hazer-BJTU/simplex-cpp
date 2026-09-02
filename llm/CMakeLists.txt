# =============================================================================
# llm — module
#
# Contract between the llm host and model-provider plugins plus the compiled
# canonical Responses-API model adapter.
# LLMModel (the three-stage model interface), LLMModelExtensionContext (the
# plugin descriptor + signed model factory), LLMDispatcher (loader + router).
# Concrete providers are plugin .so libraries of their own (installed under
# <exe>/plugins/llm) and are NOT part of this target.
#
# Dependencies:
#   - extension_framework_iface: ExtensionContext / factory resolution /
#     ExtensionDispatcher (which also carries Boost.DLL, Boost headers, the
#     third-party nlohmann/json, and the logger).
#   - dataclass_iface: the model_io data contract the interface speaks in.
#   - simplex_versioning: the build-generated LLM_PLUGIN_ABI_VERSION.
#   - asio_iface: Boost.Asio (awaitable / any_io_executor) with the shared
#     runtime — explicitly named because this module's own signatures use
#     asio directly, and every consumer must bind the one process-wide
#     runtime (utils/asio).
# =============================================================================
add_library(llm_iface INTERFACE)
target_include_directories(llm_iface INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(llm_iface INTERFACE
    extension_framework_iface
    dataclass_iface
    simplex_versioning
    asio_iface)

# The two protocol adapters are SHARED on purpose (the same treatment as
# utils/asio and utils/eventbus): statically linking them into every host
# executable *and* every provider plugin would give each module its own copy
# of the adapter code, its file-level statics, and its weak typeinfo/vtable
# emissions. Built once as libllm_responses.so / libllm_chat_completions.so:
#   - the model and exception classes' typeinfo are STRONG symbols defined in
#     exactly one object per process, so a host catching
#     ChatCompletionsApiException across the dlopen boundary matches by type
#     identity, not by the libstdc++ name-comparison fallback;
#   - the whole async stack (libasio -> libllm_* -> libeventbus) is a single
#     instance per process;
#   - plugins bind the adapter by SONAME (DT_NEEDED) instead of embedding a
#     ~MB static copy, shrinking each provider .so to its thin dialect shell.
# Visibility stays at the GCC default: hosts and plugins both subclass these
# model classes and catch their exception types, so the symbols must export.
add_library(llm_responses SHARED
    src/responses/model.cpp
    src/responses/interpreter.cpp
    src/responses/stream_handler.cpp
    src/responses/reader.cpp
)
target_include_directories(llm_responses PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(llm_responses PUBLIC llm_iface endpoint_lib)
target_compile_options(llm_responses PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
if(LTO_SUPPORT)
    set_target_properties(llm_responses PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
endif()

# Provider-neutral OpenAI-compatible Chat Completions adapter. Providers may
# derive from ChatCompletionsModel and inject a dialect; this target itself
# intentionally exports no plugin.
add_library(llm_chat_completions SHARED
    src/chat_completions/model.cpp
    src/chat_completions/interpreter.cpp
    src/chat_completions/stream_handler.cpp
    src/chat_completions/reader.cpp
)
target_include_directories(llm_chat_completions PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
# eventbus_iface: converse() broadcasts live reasoning increments on the
# process-wide bus (PUBLIC — the adapter publishes through default_bus(),
# whose singleton lives in the SHARED eventbus library; subscribers want the
# headers anyway).
target_link_libraries(llm_chat_completions
    PUBLIC llm_iface endpoint_lib eventbus_iface)
target_compile_options(llm_chat_completions PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
if(LTO_SUPPORT)
    set_target_properties(llm_chat_completions PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
endif()

add_subdirectory(providers/openai)
add_subdirectory(providers/deepseek)

# Manual demos of the provider stack; built always, not CTest-registered
# (live API key required).
add_subdirectory(example)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
