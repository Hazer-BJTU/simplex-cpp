# =============================================================================
# llm — module
#
# Header-only contract between the llm host and model-provider plugins:
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
#   - Boost::headers: Boost.Asio (awaitable / any_io_executor), explicitly
#     named because this module's own signatures use it directly.
# =============================================================================
add_library(llm_iface INTERFACE)
target_include_directories(llm_iface INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(llm_iface INTERFACE
    extension_framework_iface
    dataclass_iface
    simplex_versioning
    Boost::headers)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
