# =============================================================================
# versioning — module
#
# A parallel sibling of indextools/ that is the single source of truth for every
# project version constant. It owns no compiled code: it only feeds a header
# template (version.hpp.in) through configure_file() so the constants materialize
# as a header-only versioning/version.hpp under the build tree.
#
# Consumers get the generated header two equivalent ways:
#   - link the INTERFACE target `simplex_versioning` (preferred for targets), or
#   - use the SIMPLEX_VERSIONING_INCLUDE_DIR variable in a directory-scope
#     include_directories() (matches indextools' existing style; what indextools
#     itself uses so its host, plugins, and tests all see the header).
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
# once build/generated/ is on the include path.
set(SIMPLEX_VERSIONING_GEN_DIR "${CMAKE_BINARY_DIR}/generated")
set_property(GLOBAL PROPERTY SIMPLEX_VERSIONING_GEN_DIR "${SIMPLEX_VERSIONING_GEN_DIR}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/version.hpp.in"
    "${SIMPLEX_VERSIONING_GEN_DIR}/versioning/version.hpp"
    @ONLY)

# ---- Expose to consumers ----------------------------------------------------
# INTERFACE target: link simplex_versioning to pull in the generated include dir.
add_library(simplex_versioning INTERFACE)
target_include_directories(simplex_versioning INTERFACE
    "${SIMPLEX_VERSIONING_GEN_DIR}")

# Directory-scope variable for modules that use include_directories() rather than
# linking the target (propagates to top-level scope, hence to sibling modules
# added after this one, e.g. indextools).
set(SIMPLEX_VERSIONING_INCLUDE_DIR "${SIMPLEX_VERSIONING_GEN_DIR}" PARENT_SCOPE)
