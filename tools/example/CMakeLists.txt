# =============================================================================
# example — manual, build-only demonstrations of the tools stack.
#
# tools_deepseek_chat — the tools-stack edition of the DeepSeek chat demo: it
# keeps llm/example/deepseek_chat.cpp's provider half (LLMDispatcher loads the
# bundled provider plugins from <exe>/plugins/llm, create_model("deepseek")
# mints the model, the conversation runs through converse + integrate) and
# replaces its hand-rolled tool half (an Invocable built in the file, an
# if/else chain in execute_tool) with the shipped machinery:
#
#   ToolRegistry + ProcessToolSet + ProcessSessionStore, the four process tools
#   loaded from their YAML declarations, the set's skill (schemas/skill.yaml)
#   injected into the system prompt the same way a host injects it, and an
#   InvokeConfirmEvent handler on the process-wide async bus that asks the
#   human at the terminal before any state-changing call runs.
#
# The dependency on llm_deepseek guarantees the provider .so sits under the
# executable's plugins/llm directory, where load_default_models() looks. The
# dependency is named here even though llm/ is added AFTER tools/ at the top
# level: CMake resolves target names at generate time, not at
# add_subdirectory() time (the same reason the process toolset may link
# process_lib from a module added later).
#
# The link set is what the file actually uses, named rather than inherited:
# tools_intrinsic_process (the set + store), tools_iface through it (the
# registry, InvokeException, the security gate), llm_iface (the LLMModel
# contract + the dispatcher), llm_chat_completions (the adapter's
# reasoning-increment event the demo subscribes to), and the two event bus
# interfaces (the sync bus for reasoning, the async one for confirmations).
#
# ENABLE_EXPORTS (-rdynamic / --export-dynamic): the host executable's symbols
# open the process-wide lookup scope, so its emissions of the header-only
# contract classes (ExtensionContext / LLMModelExtensionContext / LLMModel
# typeinfo, vtables, inline members) become the authoritative copies every
# dlopened plugin binds to. Without it, exe and plugin each keep their own
# weak copy and cross-boundary dynamic_pointer_cast only works through
# libstdc++'s name-comparison fallback (name equality, not type identity).
# =============================================================================
add_executable(tools_deepseek_chat deepseek_chat.cpp)
target_compile_options(tools_deepseek_chat PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
target_link_libraries(tools_deepseek_chat PRIVATE
    tools_intrinsic_process
    llm_iface
    llm_chat_completions
    eventbus_iface
    async_eventbus_iface)
set_target_properties(tools_deepseek_chat PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(tools_deepseek_chat llm_deepseek)

if(BUILD_TESTS)
    # The OFFLINE half of the demo, and the only part of it CTest runs: --tools
    # builds the registry and the process set, loads the four declarations from
    # schemas/process/, injects the set's skill into a prompt of its own, prints
    # the catalogue and exits — no API key, no network, and no child process
    # started. It exits non-zero when a declaration failed to load, when the
    # capability group came out partial, when the routing table lost a name, or
    # when skill.yaml did not load and inject, which is what makes it a smoke
    # test of the example's own wiring rather than a print-out.
    #
    # The conversation itself is deliberately NOT registered: it needs a live
    # API key and a human answering confirmation prompts, and the tools it can
    # reach run real programs on the host.
    add_test(NAME tools_deepseek_chat_catalogue
             COMMAND tools_deepseek_chat --tools)
    set_tests_properties(tools_deepseek_chat_catalogue PROPERTIES TIMEOUT 60)
endif()
