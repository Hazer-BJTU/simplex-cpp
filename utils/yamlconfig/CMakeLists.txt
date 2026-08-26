# =============================================================================
# yamlconfig — module (utils package)
#
# One-way bridge from human-written YAML configuration to the nlohmann JSON
# the project's config contracts speak (llm model configs first). Header-only
# (yamlconfig_iface): to_json/parse/load_file plus the YamlConfigError type.
# Lives under the utils/ package; target and include paths are unchanged, so
# consumers keep linking `yamlconfig_iface` and including
# "yamlconfig/yaml_json.hpp".
#
# Dependencies: the vendored third-party tree only — yaml-cpp (headers +
# static lib) and nlohmann/json, both carried by simplex_thirdparty_iface.
# The static lib is named explicitly so the link line stays obvious even
# though the interface target already adds the search directory.
# =============================================================================
add_library(yamlconfig_iface INTERFACE)
target_include_directories(yamlconfig_iface INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(yamlconfig_iface INTERFACE simplex_thirdparty_iface yaml-cpp)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
