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
    tool_base.hpp              IntrinsicTool / DeclaredTool
    tool_declaration.hpp       the YAML declaration loader
    toolset_base.hpp
  src/  test/
  toolsets/
    process/                   process management (tools_intrinsic_process)
      include/tools/intrinsic/process/
      schemas/                 one *.yaml declaration per tool
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

**`tool_declaration.hpp` — the declaration loader.** A tool's name, description
and argument schema are the whole of what a model is told about it, and they are
a document rather than code: one YAML file per tool, kept in the toolset's own
package next to the sources it describes. `load_tool_declaration()` reads and
validates one — a mapping, with a non-empty `name`, a non-empty `description`
and an object-typed `argument_schema` — and `try_load_tool_declaration()` is the
form a tool uses, which reports the failure through the log and answers nothing
so the tool is left unnamed and its set skips it.

The schema's vocabulary is **closed**, because that subtree goes to a provider
verbatim: `type` (`string` / `boolean` / `integer` / `array`, the kinds the
argument accessors read), a `description` on every property, `default`, `enum`,
`minimum`, `minLength`, `items`, and a top-level `anyOf` for a rule that spans
properties. Each is checked against the kind it applies to and against the
others — an enum member below the declared minimum, a default outside its own
enum, an array without `items` are all refusals — and so is any keyword the
loader does not know, by name. A declaration that would reach a model as a
contract nothing here could check fails at load time instead.

What the loader does **not** read is as much a part of the design as what it
does: an `InvokeType`/`InvokeSecurity` pair may be written in the file for the
reader, and it is ignored, because those are behaviour — `write_attributes()`
owns them, and a declaration file that could quietly change a security decision
would be a way to change policy without a code review. A test pinning the file
against the implementation is what keeps the readable half honest. See
**[toolsets/process/](toolsets/process/)** for a toolset that uses it throughout.

**`tool_base.hpp` — `DeclaredTool`.** The `IntrinsicTool` whose Invocable comes
from such a file: a tool names its declaration and stops there, and everything
else — validation, the type/security pair, the work — stays in C++. A file that
cannot be loaded leaves the tool unnamed, which is how `register_tools()` skips
it rather than advertising a schema nobody could find.

**`toolset_base.hpp` — `IntrinsicToolSet`.** The ordered catalogue `get_tools()`
hands out, the name→tool table `dispatch()` routes by, and the tools'
`build()`/`release()` lifecycle. Two containers on purpose: order is what the
model reads, lookup is what routing needs — the same split `ToolRegistry` makes
one level up. A tool whose `build()` refuses, or whose name is empty or already
taken, is left out of both, so the catalogue never promises what routing cannot
answer.

A tool that does not arrive costs itself and no more — but a family of tools can
be left half-offered that way, and *that* is a state worth naming. A set may
therefore declare its **capability groups** (`declare_capability_group()`): a
group that came out partial is one error line naming the group, the count and
every missing member, and `capability_groups()` answers the same thing for a
host that wants to act on it. Registration itself stays per tool — dropping the
tools that did arrive is the host's decision, not this class's.

It deliberately does **not** override `prepare()` / `execute()`. Those carry the
invocation layer's checkpoint sequence and failure contracts (`prepare()` throws
only `InvokeException`, `execute()` never throws), and an in-process set has no
reason to want a different sequence.

A toolset package therefore supplies only its domain: its tools, their
declarations, and whatever state they share.

## Toolsets

- **[toolsets/process/](toolsets/process/)** — process management: `spawn_process`
  / `poll_processes` / `read_process_output` / `write_process_input` /
  `wait_process` / `kill_process`, over a session table that gives each child a
  name a model can return to across turns. See its own README.

## Adding a toolset

1. `toolsets/<name>/` with `include/tools/intrinsic/<name>/`, `schemas/`,
   `src/`, `test/`, a `CMakeLists.txt` and a `README.md`.
2. Derive the tools from `IntrinsicTool` (adding a family-specific base if they
   share arguments, as `ProcessToolBase` does for its session id) and the set
   from `IntrinsicToolSet` — supply `name()` and call `register_tools()`. A tool
   that declares itself in YAML derives from `DeclaredTool` instead and names its
   file; the package resolves the directory once, in a `schemas.hpp` of its own,
   and hands the library the path through a CMake compile definition with an
   environment override for deployments (see `toolsets/process/schemas.hpp`).
3. Put each tool's name, description and argument schema in its YAML file, and
   declare its `InvokeType` and `InvokeSecurity` in `write_attributes()`; check
   every argument in `ensure_arguments()` — never in `invoke()`, because the
   security check and the human confirmation must see settled arguments. Use the
   `settle_*` accessors there, so the defaults are part of the query both of
   those read. `InvokeType` describes the effect a call has OUTSIDE the host —
   the definition of the three values is at the enum itself
   (`dataclass/model_io.hpp`, `InvokeType`), because a toolset answers that
   question rather than getting its own version of it. A tool whose only changes
   are to its own component's state is `ReadOnly` **provided that component is
   safe to use concurrently**: the component owns that, the scheduler does not. A
   call that changes the world (starts something, writes to it, ends it) is a
   write, and `SerialWrite` when the order between two of them is observable out
   there.
4. Test the pair: load each declaration and ask the implementation the same
   questions the document answers (the declared kinds, defaults, enum members,
   minimums, minLengths, element types, `anyOf` alternatives and `required`) —
   each clause from BOTH sides, so "the implementation restricts something here"
   is never mistaken for "the declaration and the implementation agree". That
   check is generic over a toolset, and `toolsets/process/test/test_tools.cpp`
   is the worked example.
5. If the tools are a capability family — offered together or not at all — say so
   with `declare_capability_group()` after `register_tools()`, so a package that
   lost one declaration is reported as a degraded family rather than as healthy
   tools with one odd log line.
6. `add_subdirectory(toolsets/<name>)` in this directory's `CMakeLists.txt`;
   link `tools_intrinsic`, and build SHARED for the ABI reason below.

## Why the libraries are SHARED

Per `docs/abi-context.md`: hosts and dlopened plugins may both subclass these
classes or catch their exceptions, so the vague-linkage symbols (typeinfo,
vtables, inline members) must resolve to one authoritative copy per process.
Static linking would give every module its own copy and break
catch-by-type-identity — the same reasoning behind the shared protocol adapters
and the shared async runtime.
