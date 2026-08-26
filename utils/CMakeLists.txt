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
#   - eventbus/   `eventbus_lib` (SHARED, the default_bus() singleton) +
#                 `eventbus_iface` (INTERFACE wrapper): type-dispatched
#                 synchronous publish/subscribe bus over boost::signals2
#                 (eventbus::EventBus / eventbus::default_bus). The shared
#                 singleton gives executables and dlopened plugins one bus
#                 per process by SONAME.
#   - asio/       `asio_lib` (SHARED, the compiled Boost.Asio runtime) +
#                 `asio_iface` (INTERFACE wrapper): one copy of Asio's
#                 scheduler/reactor/SSL runtime per process by SONAME, plus
#                 the BOOST_ASIO_SEPARATE_COMPILATION switch as a usage
#                 requirement. Header-only Asio would fork per-module
#                 thread-local state across a dlopen boundary and crash the
#                 plugin's reactor callbacks intermittently.
#
# The sub-modules keep their own CMake targets and directory layout, so
# consumers keep linking `logging_lib` / `yamlconfig_iface` and including
# "logging/logger.hpp" / "yamlconfig/yaml_json.hpp" exactly as before; the
# only change is where the sources live. Add order inside the package:
# logging first (consumed by tests of both), then yamlconfig (independent),
# then eventbus (independent), then asio (depends only on top-level targets).
# =============================================================================
add_subdirectory(logging)
add_subdirectory(yamlconfig)
add_subdirectory(eventbus)
add_subdirectory(asio)
