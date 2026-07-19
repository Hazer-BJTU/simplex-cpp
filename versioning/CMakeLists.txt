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
# changes. Migrated here from indextools/include/lang_plugin.hpp (value 2); the
# lang_plugin.hpp header now aliases simplex::LANG_PLUGIN_ABI_VERSION so existing
# indextools references are unchanged.
set(SIMPLEX_LANG_PLUGIN_ABI_VERSION 2)

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
