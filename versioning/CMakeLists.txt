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
# The project version comes from the top-level project(VERSION ...); this file
# only adds module-owned constants (currently the plugin ABI version) and renders
# the header.
# =============================================================================

# ---- Module-owned version constants -----------------------------------------
# Plugin ABI contract version. Bump on binary-incompatible LangPlugin/LangAnalyze
# changes. The lang_plugin.hpp header aliases simplex::LANG_PLUGIN_ABI_VERSION so
# existing indextools references are unchanged.
#
# History:
#   v2: replaced extensions() (extension table) with file_pattern() (regex on the
#       file name) and added priority() for match ordering.
#   v3: LangPlugin now derives from the generic plugin::Plugin<LangAnalyze> base.
#       Adding that primary base reorders LangPlugin's vtable (the base virtuals
#       ~Plugin/abi_version/name/priority/create come first, before file_pattern),
#       which is binary-incompatible, so rebuilt plugins must report v3. Host and
#       plugins are always rebuilt together from the same headers, so both sides
#       agree; a stale v2 .so is deliberately rejected at load.
#   v4: LangPlugin now derives from extension::ExtensionContext (not
#       plugin::Plugin<LangAnalyze>); analyzer creation moved from the virtual
#       Plugin::create() to the exported `create_lang_analyze` alias (cached in a
#       product_factory), and the descriptor factory returns unique_ptr<ExtensionContext>.
#       New base + new members (warmed regex + product_factory) reorders the
#       layout/vtable, so rebuilt plugins must report v4.
set(SIMPLEX_LANG_PLUGIN_ABI_VERSION 4)

# LLM model-plugin ABI contract version. Bump on binary-incompatible
# llm::LLMModel / llm::LLMModelExtensionContext changes (llm/include/llm/models.hpp
# aliases it as llm::LLM_PLUGIN_ABI_VERSION, mirroring the lang-plugin pattern).
#
# History:
#   v1: first llm plugin contract. LLMModel (executor + config ctor, build/release
#       lifecycle hooks, converse/integrate virtuals) and LLMModelExtensionContext
#       (get_type/warm/create_model + the signed create_llm_model factory alias).
set(SIMPLEX_LLM_PLUGIN_ABI_VERSION 1)

# ---- Toolchain identity for the plugin admission fingerprint -----------------
# Rendered into version.hpp and hashed by extension_framework/plugin_magic.hpp
# (consteval FNV-1a, together with _GLIBCXX_USE_CXX11_ABI, sizeof(void*) and the
# build profile injected as a compile definition on extension_framework_iface).
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
