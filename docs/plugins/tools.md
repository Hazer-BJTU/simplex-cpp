# Develop a tool plugin

A dynamic tool plugin supplies one `ToolSet`. The set advertises model-facing
`Invocable` declarations and dispatches queries to owned `ToolInterface` handles.
The registry performs the surrounding authorization and scheduling work.

## Factories

Include `tools/extensions/plugin.hpp` and `extensions/plugin_magic.hpp`.
Export these signatures:

```cpp
std::unique_ptr<extension::ExtensionContext> create_toolset_plugin();
std::unique_ptr<tools::ToolSet> create_toolset(
    const tools::extensions::ToolSetConfig& config);
```

The descriptor derives from `ToolSetExtensionContext`, reports
`tools::extensions::kAbiVersion`, and returns a stable name. Register both
Boost.DLL aliases and the plugin magic block. The
[complete noop implementation](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/tools/extensions/stubs/noop/plugin.cpp)
shows the exact interface overrides and ownership.

## CMake and schema layout

```cmake
add_library(example_toolset MODULE plugin.cpp)
target_link_libraries(example_toolset PRIVATE tools_extensions boost_dll_iface)
set_target_properties(example_toolset PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/tools")
simplex_install_tool_extension_schema(
    example_toolset "${CMAKE_CURRENT_SOURCE_DIR}/schemas")
```

Create `schemas/config.yaml`:

```yaml
name: example_toolset
description: A diagnostic toolset.
config: {}
```

Create one declaration file per tool, such as `schemas/example_probe.yaml`:

```yaml
name: example_probe
description: Return a diagnostic acknowledgement without external effects.
argument_schema:
  type: object
  required: []
  properties: {}
```

An optional `schemas/skill.yaml` adds model-facing usage guidance:

```yaml
name: example_toolset
title: Diagnostic probe
text: |
  Use example_probe to check whether this toolset is available.
```

Use `tools::extensions::load_tool(config, "example_probe")` and
`load_skill(config)` to reuse declaration validation. A missing optional skill
returns `nullopt`; malformed present YAML is an error. The schema install helper
copies files into the build tree and installed executable-relative schema tree.

## Tool behavior

Implement `get_details()`, `write_attributes()`, and asynchronous `invoke()`.
`write_attributes()` sets runtime invocation and security classifications; YAML
alone does not enforce side effects or authorization. The wire spelling
`parall_write` is intentional. Choose read-only, parallel-write, or serial-write
behavior according to actual effects and shared state.

Implement `ToolSet::name()`, `get_tools()`, `dispatch()`, and optional `skill()`.
Do not advertise a tool whose implementation or declaration is missing. Retain
tool instances with managed handles so they can outlive a dispatch lookup safely.

Return a `model_io::Content` describing the result. State side effects clearly,
including partial work before failure. Use the shared
[text-format utilities](https://github.com/Hazer-BJTU/simplex-cpp/tree/main/utils/textformat)
when matching built-in metadata/output formatting. Do not launch detached work
that outlives the tool without an explicitly owned cleanup path.

Finally enable the descriptor in `plugins.extensions.tools.enable`, restart the
worker, and check discovery and invocation logs.

## Per-tool host settings

Intrinsic declarations can include an optional host-side `config` mapping.
See [intrinsic tool configuration](configuration.md#intrinsic-tool-configuration)
for the lifecycle and `read_text` example. A `DeclaredTool` implementation calls
`initialize_configuration()` in its derived constructor body, validates every
key/type/range, and commits typed settings only after validation. Use
`configuration_error(field, reason)` for diagnostics naming the YAML file and
`/config/<field>`, without dumping values. The initializer catches failures and
clears the tool's advertised name so registration skips partial initialization.
Custom `build()` overrides must first call `DeclaredTool::build()`; the base also
refuses nonempty settings that no concrete initializer consumed.

`tools::extensions::load_tool()` returns only `Invocable` and therefore refuses
nonempty per-tool settings. A dynamic tool needing these settings can use
`intrinsic::load_tool_declaration()` and validate its separate `config` member;
its existing toolset-level configuration is not implicitly merged. Rebuild tool
plugins against toolset ABI **4** after this shared helper layout change.
