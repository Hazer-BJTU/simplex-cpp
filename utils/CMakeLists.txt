# =============================================================================
# utils — package
#
# Umbrella for the small shared utility modules every other package leans on:
#
#   - logging/    `logging_lib` (STATIC): thread-safe console logger
#                 (logging::Logger), standard-library only.
#   - yamlconfig/ `yamlconfig_iface` (INTERFACE, header-only): one-way
#                 YAML -> nlohmann JSON config bridge (yamlconfig::*) over
#                 the vendored yaml-cpp + nlohmann/json tree.
#   - eventbus/   `eventbus_iface` (INTERFACE, header-only): type-dispatched
#                 synchronous publish/subscribe bus over boost::signals2
#                 (eventbus::EventBus / eventbus::default_bus).
#
# The sub-modules keep their own CMake targets and directory layout, so
# consumers keep linking `logging_lib` / `yamlconfig_iface` and including
# "logging/logger.hpp" / "yamlconfig/yaml_json.hpp" exactly as before; the
# only change is where the sources live. Add order inside the package:
# logging first (consumed by tests of both), then yamlconfig (independent),
# then eventbus (independent).
# =============================================================================
add_subdirectory(logging)
add_subdirectory(yamlconfig)
add_subdirectory(eventbus)
