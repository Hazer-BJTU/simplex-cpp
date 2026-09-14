# =============================================================================
# yamlconfig — module (utils package)
#
# One-way bridge from human-written YAML configuration to the nlohmann JSON the
# project's config contracts speak (llm model configs first, the tool
# declarations and the host config after it): parse()/load_file() plus the
# YamlConfigError type. Lives under the utils/ package; target and include paths
# are unchanged, so consumers keep linking `yamlconfig_iface` and including
# "yamlconfig/yaml_json.hpp".
#
# SHARED, and this is the change that made the module a library rather than a
# header: `yamlconfig_lib` owns the conversion and the ONE copy of yaml-cpp per
# process, while `yamlconfig_iface` remains what a consumer links (include dir +
# the library). Both halves are the tree's usual reasons, stated in full in the
# header's file comment:
#
#   - YamlConfigError is caught across module boundaries (a plugin catching what
#     a host's config load threw), so it must have one typeinfo per process.
#     Declaring its destructor gives the class a key function, which is emitted
#     here and nowhere else.
#   - yaml-cpp used to be compiled into EVERY consumer that included the header,
#     and its symbols were then re-exported by each of them (a static archive
#     linked into a shared object is public by default). One module linking it
#     is what makes "one copy per process" true rather than aspirational.
#
# `--exclude-libs,ALL` hides the symbols the STATIC ARCHIVE contributes — the
# objects yaml-cpp itself was compiled into — from this library's dynamic symbol
# table. It cannot hide what a translation unit emits from yaml-cpp's HEADERS:
# the template and inline instantiations (120 weak symbols) stay exported, and
# no linker option changes that short of a version script or explicit
# visibility control, neither of which this tree uses anywhere. So the
# guarantee this module actually provides is the other half of the pair, and it
# is the stronger one: it is the ONLY module that includes yaml-cpp's headers
# (the public header does not, and nothing else in the tree does), so no other
# object defines those weak symbols and there is nothing for them to interpose
# with. Verified with `nm -D --defined-only build/lib/*.so`: libyamlconfig.so is
# the only one that defines a single YAML:: symbol. Our own objects
# (`yamlconfig::parse`, `load_file`, `YamlConfigError`) are unaffected.
#
# Dependencies: the vendored third-party tree only — yaml-cpp (headers + static
# lib) and nlohmann/json, both carried by simplex_thirdparty_iface. PRIVATE, so
# a consumer gets nlohmann's headers (this module's own public header needs
# them) but never yaml-cpp's: nothing outside this module names it.
# =============================================================================
add_library(yamlconfig_lib SHARED src/yaml_json.cpp)
target_include_directories(yamlconfig_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(yamlconfig_lib PROPERTIES OUTPUT_NAME yamlconfig)
target_link_libraries(yamlconfig_lib PRIVATE simplex_thirdparty_iface yaml-cpp)
target_link_options(yamlconfig_lib PRIVATE "LINKER:--exclude-libs,ALL")
target_compile_options(yamlconfig_lib PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
if(LTO_SUPPORT)
    set_target_properties(yamlconfig_lib PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
endif()

add_library(yamlconfig_iface INTERFACE)
target_include_directories(yamlconfig_iface INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(yamlconfig_iface INTERFACE yamlconfig_lib simplex_thirdparty_iface)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
