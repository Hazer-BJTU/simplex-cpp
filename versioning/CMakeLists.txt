# =============================================================================
# versioning — module
#
# A sibling module that is the single source of truth for every project version
# constant. It owns no compiled code: it only feeds a header template
# (version.hpp.in) through configure_file() so the constants materialize as a
# header-only versioning/version.hpp under the build tree.
#
# Consumers get the generated header by linking the INTERFACE target
# `simplex_versioning` — that is the single, supported way (the project manages
# all header paths through interface targets, not directory-scope variables).
#
# The project version comes from root VERSION through the top-level project()
# call; this file adds independent LLM, toolset, and loop-hook ABI constants
# and renders the header.
# =============================================================================

# ---- Module-owned version constants -----------------------------------------
# LLM model-plugin ABI contract version. Bump on binary-incompatible
# llm::LLMModel / llm::LLMModelExtensionContext changes (llm/include/llm/models.hpp
# aliases it as llm::LLM_PLUGIN_ABI_VERSION).
#
# History:
#   v1: first llm plugin contract. LLMModel (executor + config ctor, build/release
#       lifecycle hooks, converse/integrate virtuals) and LLMModelExtensionContext
#       (get_type/warm/create_model + the signed create_llm_model factory alias).
#   v2: LLMModel gained provider_info() (async catalogue query returning a JSON
#       array), the two set_generation() tiers (typed GenerationPreset + JSON
#       merge-patch), and the generation() accessor; the _generation member and
#       its derivation moved from the protocol adapters up into the base (new
#       contract types: ReasoningEffort, GenerationPreset). New virtuals reorder
#       the vtable, so rebuilt plugins must report v2.
#   v3: ChatCompletionsDialect gained balance_path() (the account-balance
#       companion provider_info() attaches beside the models catalogue). The
#       dialect vtable is consumed across the plugin boundary — the adapter's
#       provider_info() calls the hook on an object constructed inside the
#       provider .so — so the appended slot makes a stale plugin
#       binary-incompatible; rebuilt plugins must report v3.
#   v4: converse() became REENTRANT, which changed both its signature and the
#       base layout. converse(const AgentInputState&) -> converse(AgentInputState)
#       (a coroutine must own its argument, not borrow it across the first
#       suspension) is a different vtable slot signature; _generation went
#       private behind a std::shared_mutex with generation_snapshot() /
#       reset_generation() accessors, which both grows and reorders LLMModel.
#       generation() now returns by value (a reference would race a concurrent
#       set_generation()). A stale plugin would mis-call the virtual and
#       mis-locate every member past _config, so rebuilt plugins must report v4.
#   v5: AgentInputState gained optional loop recovery data. Its layout crosses
#       converse()/integrate() and must agree between host and model plugins.
#   v6: LoopProgress and AgentLoopStep gained durable response commit sequences.
#       Both layouts cross the same model plugin boundary.
#   v7: LLMModel gained the const virtual get_options() metadata query.
#       Host and providers must be rebuilt together for the new vtable slot.
#   v8: LLMModel gained the synchronous virtual handle_options() mutation hook.
#   v9: LLMModel gained the const virtual get_current_options() query.
#   v10: model_io::Content gained an explicit Modality label, which changed both
#       its size and its member order. Content is a member of MessageItem and
#       AgentInputState, so every boundary that carries those — converse(),
#       integrate(), the loop-hook events, and the toolset calls below — now
#       disagrees with a plugin built against v9 about where each member after
#       `raw` begins. Rebuilt plugins must report v10.
set(SIMPLEX_LLM_PLUGIN_ABI_VERSION 10)

# Independent contracts for dynamic toolsets and loop hooks.
# v2: shared intrinsic ToolResult changed layout to buffer metadata/output
#     separately. Tool plugins using that helper must be rebuilt with the host.
# v3: model_io::Content gained the Modality member. ToolInterface::invoke()
#     returns a Content by value and check_result() takes one by value, so a
#     toolset built against v2 would construct and read a different object.
# v4: ToolDeclaration carries owned host configuration; DeclaredTool retains
#     configuration and its source/validation state. Dynamic tools subclassing
#     these shared helpers or returning declarations must be rebuilt together.
set(SIMPLEX_TOOLSET_PLUGIN_ABI_VERSION 4)

# v2: loop hook events hand out MessageItem and AgentInputState references
#     (loop/events.hpp), both of which contain Content. The same layout change
#     therefore reaches every hook implementation, not only the ones that name
#     Content directly.
set(SIMPLEX_LOOP_HOOK_PLUGIN_ABI_VERSION 3)

# ---- Toolchain identity for the plugin admission fingerprint -----------------
# Rendered into version.hpp and hashed by extensions/plugin_magic.hpp
# (consteval FNV-1a, together with _GLIBCXX_USE_CXX11_ABI, sizeof(void*) and the
# build profile injected as a compile definition on extensions_iface).
# find_package(Boost) at the top level runs before this subdirectory, so
# Boost_VERSION is available here.
set(SIMPLEX_PLUGIN_TOOLCHAIN_ID
    "${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")
set(SIMPLEX_PLUGIN_CXX_STANDARD "C++${CMAKE_CXX_STANDARD}")
set(SIMPLEX_PLUGIN_BOOST_VERSION "${Boost_VERSION}")

# ---- Render the generated header --------------------------------------------
# Generated under build/generated/ so it is found as "versioning/version.hpp"
# once build/generated/ is on the include path (carried by simplex_versioning).
set(SIMPLEX_VERSIONING_GEN_DIR "${CMAKE_BINARY_DIR}/generated")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/version.hpp.in"
    "${SIMPLEX_VERSIONING_GEN_DIR}/versioning/version.hpp"
    @ONLY)

# ---- Expose to consumers ----------------------------------------------------
# INTERFACE target: link simplex_versioning to pull in the generated include dir.
add_library(simplex_versioning INTERFACE)
target_include_directories(simplex_versioning INTERFACE
    "${SIMPLEX_VERSIONING_GEN_DIR}")
