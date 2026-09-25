# =============================================================================
# utils — package
#
# Umbrella for the small shared utility modules every other package leans on:
#
#   - textformat/ `textformat_iface` (INTERFACE): dependency-free plain
#                 metadata field formatting.
#   - logging/    `logging_lib` (STATIC): thread-safe console logger
#                 (logging::Logger), standard-library only.
#   - fileio/     `fileio_lib` (SHARED): atomic file replacement through a
#                 stream callback, with private POSIX resource ownership.
#   - yamlconfig/ `yamlconfig_lib` (SHARED, the one copy of yaml-cpp per
#                 process and of the conversion itself) + `yamlconfig_iface`
#                 (INTERFACE wrapper): one-way YAML -> nlohmann JSON config
#                 bridge (yamlconfig::parse / load_file / YamlConfigError).
#                 Shared because the error type crosses module boundaries and
#                 because a header-only bridge would compile — and re-export —
#                 the vendored yaml-cpp in every consumer.
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
#   - async_eventbus/ `async_eventbus_lib` (SHARED, the default_async_bus()
#                 singleton) + `async_eventbus_iface` (INTERFACE wrapper):
#                 the coroutine counterpart of eventbus — handlers are
#                 boost::asio::awaitable<Evt>(const Evt&) and publish() folds
#                 the event through them serially (each handler's co_return
#                 becomes the next handler's input), with token-selected
#                 propagate/capture exception modes. Links asio_iface.
#
# The sub-modules keep their own CMake targets and directory layout, so
# consumers keep linking `logging_lib` / `yamlconfig_iface` and including
# "logging/logger.hpp" / "yamlconfig/yaml_json.hpp" exactly as before; the
# only change is where the sources live. Add order inside the package:
# logging first (consumed by tests of both), then yamlconfig (independent),
# then eventbus (independent), then asio (depends only on top-level targets),
# then async_eventbus (depends on asio_iface).
# =============================================================================
add_subdirectory(textformat)
add_subdirectory(logging)
add_subdirectory(fileio)
add_subdirectory(yamlconfig)
add_subdirectory(eventbus)
add_subdirectory(asio)
add_subdirectory(async_eventbus)
