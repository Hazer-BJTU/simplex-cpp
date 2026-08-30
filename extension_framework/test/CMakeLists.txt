# Unit tests for the extension framework.
#
# Two test binaries, each with its own BOOST_TEST_MODULE (Boost.Test static
# linking needs one main per binary):
#
#   test_extensions          — pure-logic / in-memory coverage of the header:
#                              is_likely_dynamic_library, same_tag_always,
#                              ExtensionContext (bind/priority/extras),
#                              get_library_ref error path, create_object_from_library
#                              null/missing-symbol paths, verify_after_loaded
#                              filtering + sorting + parallel-errors, and
#                              load_modules_directory error collection (using a
#                              runtime-created bogus .so, no build-time DSO).
#
#   test_extensions_dynamic  — drives the real dynamic-loading pipeline over a
#                              toy DSO (libtoyextension.so): get_library_ref +
#                              create_object_from_library + bind, the deleter-pinned
#                              mode, wrong-alias rejection, and a full
#                              load_modules_directory + verify_after_loaded pass.
#
# The toy DSO is emitted into bin/extension_test_modules (NOT bin/plugins) so the
# real host's default-directory scan never sees it. The directory name is chosen
# to differ from the test executable names (bin/test_extensions etc.) so the DSO
# output directory never collides with an executable output path.
#
# extension_framework_iface already links boost_dll_iface (BOOST_DLL_USE_STD_FS +
# filesystem + dl), logging_lib, and simplex_thirdparty_iface, so the tests and
# the toy module need no extra dependency wiring.

set(_ext_test_opts
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)

# --- toy DSO for the dynamic-loading test ------------------------------------
# A separate domain (its own ExtensionContext subclass, ABI, factory alias)
# proving the framework is not coupled to any concrete product type.
add_library(toyextension MODULE toy_extension.cpp)
target_include_directories(toyextension PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(toyextension PRIVATE ${_ext_test_opts})
target_link_libraries(toyextension PRIVATE extension_framework_iface)
set_target_properties(toyextension PROPERTIES
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/extension_test_modules")

# --- admission-gate fixtures --------------------------------------------------
# Modules for the toolchain-fingerprint admission tests: one carrying a
# deliberately wrong fingerprint, one with no magic block at all (the
# legacy/hand-rolled shape). Kept in their own directory so the
# directory-scan tests above still see exactly one module.
foreach(_fixture bad_magic legacy)
    set(_fixture_target toyextension_${_fixture})
    set(_fixture_source toy_extension_${_fixture}.cpp)
    add_library(${_fixture_target} MODULE ${_fixture_source})
    target_include_directories(${_fixture_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options(${_fixture_target} PRIVATE ${_ext_test_opts})
    target_link_libraries(${_fixture_target} PRIVATE extension_framework_iface)
    set_target_properties(${_fixture_target} PROPERTIES
        PREFIX "lib"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/extension_test_modules_bad")
    unset(_fixture_target)
    unset(_fixture_source)
endforeach()

# --- pure-logic tests --------------------------------------------------------
add_executable(test_extensions test_extensions.cpp)
target_include_directories(test_extensions PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(test_extensions PRIVATE ${_ext_test_opts})
target_link_libraries(test_extensions PRIVATE extension_framework_iface Boost::unit_test_framework)
add_test(NAME test_extensions COMMAND test_extensions)

# --- dynamic-loading tests ---------------------------------------------------
add_executable(test_extensions_dynamic test_extensions_dynamic.cpp)
target_include_directories(test_extensions_dynamic PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(test_extensions_dynamic PRIVATE ${_ext_test_opts})
target_link_libraries(test_extensions_dynamic PRIVATE extension_framework_iface Boost::unit_test_framework)
target_compile_definitions(test_extensions_dynamic PRIVATE
    "TOY_EXTENSION_DIR=\"${CMAKE_BINARY_DIR}/bin/extension_test_modules\""
    "TOY_BAD_MAGIC_DIR=\"${CMAKE_BINARY_DIR}/bin/extension_test_modules_bad\"")
# ENABLE_EXPORTS (-rdynamic): exports the executable's symbols into the
# process-wide lookup scope, so the dlopened toy module binds the exe's
# authoritative copies of the header-only ExtensionContext typeinfo/vtable
# (type identity, not the libstdc++ name-comparison fallback). The same
# treatment is applied to every host that dlopens plugins.
set_target_properties(test_extensions_dynamic PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(test_extensions_dynamic toyextension toyextension_bad_magic toyextension_legacy)
add_test(NAME test_extensions_dynamic COMMAND test_extensions_dynamic)
