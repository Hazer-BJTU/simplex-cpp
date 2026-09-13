# tools/intrinsic

The host's own built-in tools — the ones a host offers without loading
anything, as opposed to the plugin-provided tools the extension framework
brings.

Organised the way `llm/` organises its providers: a shared core at this level,
one self-contained package per toolset under `toolsets/`, each with its own
`include/` + `src/` + `test/` and its own library. One deliberate difference
from a provider, though — **an intrinsic toolset is not a plugin.** There is no
`ExtensionContext`, no signed factory and no `dlopen`: a toolset is a shared
library the host links directly, constructs, and hands to its `ToolRegistry`.
The plugin machinery buys dynamic discovery, which is exactly what "built in"
does not need.

```
tools/intrinsic/
  include/tools/intrinsic/     the shared core (tools_intrinsic)
    tool_base.hpp
    toolset_base.hpp
  src/  test/
  toolsets/
    process/                   process management (tools_intrinsic_process)
      include/tools/intrinsic/process/
      src/  test/  README.md
```

## The shared core — `tools_intrinsic` (libintrinsic_tools.so)

What a toolset would otherwise re-solve, so the next family inherits it instead
of copying it. It plays the part `llm_iface` plus the protocol adapters play for
a provider.

**`tool_base.hpp` — `IntrinsicTool`.** The `Invocable`'s storage (`get_details()`
returns a reference by contract, so the description has to live somewhere the
tool owns); the argument accessors; the JSON result shape; the JSON-Schema
builders; and the routing of a `RequireConfirm` confirmation at a chosen event
bus, so a component — or a test — can keep its confirmations to itself rather
than subscribing to the process-wide bus where a handler would answer for
everyone.

The accessors **refuse rather than coerce**, which is the rule worth knowing
before writing a tool: a `"false"` string is truthy under every coercion rule,
so a coerced boolean hands a model the opposite of what it asked for with
nothing in the result to explain it. Integers accept both of nlohmann's integer
kinds and check the sign separately — a plain positive literal is stored as
*signed*, so a check written only against `is_number_unsigned()` rejects every
ordinary value. Lists are checked element by element so a bad entry names its
own index.

They come in two families, and `ensure_arguments()` wants the second:
`optional_*` **read** (for `invoke()`), and `settle_*` read, validate and
**write the default back into the query**. The settled query is what the
security policy judges, what a human confirmer is shown, what `invoke()` reads
and what the returned record carries — so a default the tool merely knew about
would mean all four saw a different call from the one that ran. A call whose
`arguments` is not a JSON object is refused rather than settled as "every
property absent".

**`toolset_base.hpp` — `IntrinsicToolSet`.** The ordered catalogue `get_tools()`
hands out, the name→tool table `dispatch()` routes by, and the tools'
`build()`/`release()` lifecycle. Two containers on purpose: order is what the
model reads, lookup is what routing needs — the same split `ToolRegistry` makes
one level up. A tool whose `build()` refuses, or whose name is empty or already
taken, is left out of both, so the catalogue never promises what routing cannot
answer.

It deliberately does **not** override `prepare()` / `execute()`. Those carry the
invocation layer's checkpoint sequence and failure contracts (`prepare()` throws
only `InvokeException`, `execute()` never throws), and an in-process set has no
reason to want a different sequence.

A toolset package therefore supplies only its domain: its tools, their schemas,
and whatever state they share.

## Toolsets

- **[toolsets/process/](toolsets/process/)** — process management: `spawn_process`
  / `poll_processes` / `read_process_output` / `write_process_input` /
  `wait_process` / `kill_process`, over a session table that gives each child a
  name a model can return to across turns. See its own README.

## Adding a toolset

1. `toolsets/<name>/` with `include/tools/intrinsic/<name>/`, `src/`, `test/`,
   a `CMakeLists.txt` and a `README.md`.
2. Derive the tools from `IntrinsicTool` (adding a family-specific base if they
   share arguments, as `ProcessToolBase` does for its session id) and the set
   from `IntrinsicToolSet` — supply `name()` and call `register_tools()`.
3. Declare each tool's `InvokeType` and `InvokeSecurity` in
   `write_attributes()`, and check every argument in `ensure_arguments()` —
   never in `invoke()`, because the security check and the human confirmation
   must see settled arguments. Use the `settle_*` accessors there, so the
   defaults are part of the query both of those read. `InvokeType` describes the
   effect a call has OUTSIDE the host: a tool whose only changes are to its own
   component's state is `ReadOnly` **provided that component is safe to use
   concurrently** — the component owns that, the scheduler does not. A call that
   changes the world (starts something, writes to it, ends it) is a write, and
   `SerialWrite` when the order between two of them is observable out there.
4. `add_subdirectory(toolsets/<name>)` in this directory's `CMakeLists.txt`;
   link `tools_intrinsic`, and build SHARED for the ABI reason below.

## Why the libraries are SHARED

Per `docs/abi-context.md`: hosts and dlopened plugins may both subclass these
classes or catch their exceptions, so the vague-linkage symbols (typeinfo,
vtables, inline members) must resolve to one authoritative copy per process.
Static linking would give every module its own copy and break
catch-by-type-identity — the same reasoning behind the shared protocol adapters
and the shared async runtime.
