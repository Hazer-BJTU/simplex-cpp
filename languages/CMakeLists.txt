message(STATUS "build language support (plugin mode)")

# ============================================================================ #
# Language plugin auto-discovery                                               #
# ============================================================================ #
#
# Every immediate subdirectory that contains a CMakeLists.txt is treated as a
# self-contained language module and added to the build, EXCEPT reserved support
# directories (currently `test/`). Adding a new language therefore requires NO
# edit to this file (or any file above it): just drop a new module directory
# next to python/ and fallback/. See README.md for the module contract.
#
# Modules cooperate with the rest of the build through two GLOBAL properties,
# which they append to (never the parent editing them):
#
#   INDEX_LANG_PLUGINS       - plugin (MODULE) targets, one .so per language,
#                                emitted into bin/plugins/. The host adds these
#                                as build dependencies so `make` produces them.
#   INDEX_LANG_ANALYZER_LIBS - static analyzer libs used by the unit tests
#                                that include an analyzer header directly.
#
# Initialize both to empty so consumers can read them unconditionally.
set_property(GLOBAL PROPERTY INDEX_LANG_PLUGINS "")
set_property(GLOBAL PROPERTY INDEX_LANG_ANALYZER_LIBS "")

# Directories that live under languages/ but are NOT language modules.
set(_LANG_NON_MODULE_DIRS test)

file(GLOB _lang_entries LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/*")
foreach(_entry ${_lang_entries})
    if(IS_DIRECTORY "${_entry}" AND EXISTS "${_entry}/CMakeLists.txt")
        get_filename_component(_lang_name "${_entry}" NAME)
        if(_lang_name IN_LIST _LANG_NON_MODULE_DIRS)
            continue()
        endif()
        message(STATUS "  language module: ${_lang_name}")
        add_subdirectory("${_entry}")
    endif()
endforeach()

# Language analyzer unit tests (e.g. test_pythonlang). Built after the language
# modules above, so INDEX_LANG_ANALYZER_LIBS is already populated. BUILD_TESTS
# is a project-wide option declared at the top level.
if(BUILD_TESTS)
    add_subdirectory(test)
endif()
