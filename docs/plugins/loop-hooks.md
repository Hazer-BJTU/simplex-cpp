# Develop a loop hook

A hook attaches synchronous handlers to the bus used by `loop::run`. It can
observe progress or edit state only through events that explicitly permit it.
A session-level `LoopHookRegistry` owns instances and their subscription bindings.

## Factories and configuration

```cpp
std::unique_ptr<extension::ExtensionContext> create_loop_hook_plugin();
std::unique_ptr<loop::LoopHookInterface> create_loop_hook(
    const loop::intrinsic::HookConfig& config);
```

The descriptor derives from `loop::extensions::LoopHookExtensionContext` and
reports `loop::extensions::kAbiVersion`. Export the two aliases and the magic
block. Configuration is `name`, `description`, and `config: {}`; validate every
plugin-specific option before returning the instance.

```cmake
add_library(example_hook MODULE plugin.cpp)
target_link_libraries(example_hook PRIVATE loop_extensions boost_dll_iface)
set_target_properties(example_hook PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/loop")
simplex_install_loop_extension_config(
    example_hook "${CMAKE_CURRENT_SOURCE_DIR}/schemas/config.yaml")
```

## Own subscriptions immediately

This example shows the essential override, not the descriptor/factory boilerplate:

```cpp
class ExampleHook final : public loop::LoopHookInterface {
public:
    std::string_view name() const noexcept override { return "example_hook"; }
protected:
    Subscriptions subscribe(eventbus::EventBus& bus) override {
        Subscriptions result;
        eventbus::EventBus::ScopedSubscription owned{
            bus.subscribe<loop::RunStarted>([](const loop::RunStarted&) {
                // Observe this event synchronously.
            })};
        result.push_back(std::move(owned));
        return result;
    }
};
```

Adopting the connection before vector insertion is important: allocation can
throw after subscription succeeds. A raw connection left behind could later call
a destroyed object. Follow the
[complete noop hook](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/loop/extensions/stubs/noop/plugin.cpp)
for includes, factory validation, and exports.

## Callback and edit rules

Callbacks run inline and must not suspend, retain event references, access live
state later on another thread, or recursively enter the loop. The bus must outlive
the registry. Serialize registry changes with run/event publication; disconnecting
a callback does not by itself join an invocation already executing.

`EditOnStepFinished` permits in-place AgentInputState changes after tool results
are committed. `EditOnRunFinished` offers the corresponding end-of-run boundary.
Use them for context pruning or persistent statistics while preserving state
integrity. With subscribers, the loop takes one event-wide state backup; failed
validation or an exception restores it by move assignment. Successful edits
remain in place. Progress fields are reserved, and tool calls/results must stay
paired; recovery history in Projection/Blocked cannot be rewritten. Invalid edits and throwing callbacks follow each event's validation
and rollback rules; inspect
[loop/events.hpp](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/loop/include/loop/events.hpp)
before mutating state. Avoid unnecessary copies of the entire conversation.

Instance fields may cache process-local information. Values needed after restart
belong in AgentInputState, typically namespaced extras. The built-in context
statistic hook demonstrates writing structured external status without making
its own instance a persistence object.

Enable the hook in `plugins.extensions.loop_hooks.enable`. Hooks are constructed
in configured order and registered after intrinsic hooks.
