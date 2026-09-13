# =============================================================================
# llm — module
#
# Contract between the llm host and model-provider plugins, plus the
# compatibility layer that speaks the external provider protocols.
# LLMModel (the three-stage model interface), LLMModelExtensionContext (the
# plugin descriptor + signed model factory), LLMDispatcher (loader + router).
# Concrete providers are plugin .so libraries of their own (installed under
# <exe>/plugins/llm) and are NOT part of this target.
#
# THIS DIRECTORY OWNS THE MODULE CORE. include/llm/ here is the provider-neutral
# contract every host and plugin compiles against (models.hpp,
# provider_models.hpp, exchange_id.hpp) and nothing protocol-specific lives in
# it. The two wire protocols are implemented in the compatibility layer —
# compat/, with its own README, its own libraries and its own tests — because
# they are the part of this module whose shape is dictated from outside this
# repository, and the core must not depend on either of them.
#
# Dependencies:
#   - extensions_iface: ExtensionContext / factory resolution /
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
    extensions_iface
    dataclass_iface
    simplex_versioning
    asio_iface)

# The two protocol adapters (the compatibility layer). Their SHARED-ness and
# the ABI reasoning behind it live with them, in compat/CMakeLists.txt.
add_subdirectory(compat)

add_subdirectory(providers/openai)
add_subdirectory(providers/deepseek)

# Manual demos of the provider stack; built always, not CTest-registered
# (live API key required).
add_subdirectory(example)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
