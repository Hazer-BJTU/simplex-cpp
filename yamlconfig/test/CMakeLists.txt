# Unit tests for yamlconfig/yaml_json.hpp (YAML -> nlohmann JSON).
#
# test_yaml_json — the conversion matrix: the scalar table (YAML 1.1
# booleans, quoted numbers staying strings, int64/uint64 band, finite
# doubles), nesting, anchors/aliases, merge keys, textualised keys, the
# error paths (non-finite numbers, non-scalar keys, multiple documents,
# syntax errors carrying marks, cyclic aliases, missing files), the file
# entry point over a temp file, and a realistic llm-style config checked
# field-by-field plus a dump()/re-parse legality round-trip.
#
# yamlconfig_iface carries yaml-cpp + nlohmann/json transitively.

add_executable(test_yaml_json test_yaml_json.cpp)
target_compile_options(test_yaml_json PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
target_link_libraries(test_yaml_json PRIVATE yamlconfig_iface Boost::unit_test_framework)
add_test(NAME test_yaml_json COMMAND test_yaml_json)
