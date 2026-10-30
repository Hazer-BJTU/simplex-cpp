# =============================================================================
# glm — provider plugin (llm package)
#
# GLM (Zhipu AI / BigModel) on the provider-neutral chat-completions adapter
# (llm_chat_completions): the platform's endpoint defaults, the always-on
# thinking policy of the GLM-5.3 family, the effort translation and the
# finish-reason vocabulary live in the header-only dialect
# (include/llm/glm/dialect.hpp); the plugin .cpp itself is only the model
# subclass and the two standard factory aliases. Emitted next to llm_openai
# under <exe>/plugins/llm.
#
# The provider-local tests exercise the dialect (offline) and the full
# converse() exchange against a loopback server, so this directory needs the
# test subdirectory gated like every other module.
# =============================================================================
add_library(llm_glm MODULE src/glm_plugin.cpp)
target_include_directories(llm_glm PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
# PUBLIC, not PRIVATE: this plugin's own public header
# (include/llm/glm/dialect.hpp) includes the adapter's
# llm/compat/chat_completions/dialect.hpp, so the adapter's include directory is
# part of what this target's interface needs. Nothing links a MODULE library,
# so nothing inherits it today; the declaration is what keeps the dependency
# visible instead of ambient.
target_link_libraries(llm_glm PUBLIC llm_chat_completions boost_dll_iface)
target_compile_options(llm_glm PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
)
set_target_properties(llm_glm PROPERTIES
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/llm")
if(LTO_SUPPORT)
    set_target_properties(llm_glm PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
endif()

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
