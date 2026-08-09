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
    POSITION_INDEPENDENT_CODE ON
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/extension_test_modules")

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
    "TOY_EXTENSION_DIR=\"${CMAKE_BINARY_DIR}/bin/extension_test_modules\"")
add_dependencies(test_extensions_dynamic toyextension)
add_test(NAME test_extensions_dynamic COMMAND test_extensions_dynamic)
