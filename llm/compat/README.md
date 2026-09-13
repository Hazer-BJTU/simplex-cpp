# The LLM compatibility layer

`llm/compat` holds the two adapters that speak an **external wire protocol** —
OpenAI-compatible Chat Completions (`chat_completions/`) and the Responses API
(`responses/`) — as submodules of the `llm` module. Each has its own include
directory, its own shared library and its own test suite, and neither is part of
the module core.

## Why these two are a layer

They are the only part of `llm` whose shape is dictated from outside this
repository. Everything else in the module is a contract this project owns:
`LLMModel`, the plugin descriptor, the dispatcher, and the canonical data
(`model_io`) the host speaks in. A wire protocol is not ours to change — it is
what a provider already serves and what a model's stream already looks like —
so the code that maps it is grouped where that fact is visible:

- it depends **downward** on `endpoint` (HTTP/SSE transport) and on the module
  core, never the other way round: `llm/include/llm/` is protocol-neutral and
  stays compilable without either adapter;
- the two adapters are peers, not layers over each other. `responses` is the
  canonical one the module ships a model for; `chat_completions` is the
  compatible one most providers actually expose, with a dialect seam
  (`ChatCompletionsDialect`) for their deviations;
- a provider plugin picks exactly one of them to build on (that is the whole
  difference between `llm_openai` and `llm_deepseek`) and inherits only that
  adapter's runtime.

## Layout

```text
llm/
  include/llm/                     the module core, all that llm_iface exposes
    models.hpp                     LLMModel / the plugin ABI / LLMDispatcher
    provider_models.hpp            the provider catalogue normaliser
    exchange_id.hpp                the per-exchange correlation id
  compat/                          ← this layer
    chat_completions/
      include/llm/compat/chat_completions/  8 public headers
      src/                                  model, interpreter, stream handler, reader
      test/                                 5 test executables
    responses/
      include/llm/compat/responses/         8 public headers
      src/                                  model, interpreter, stream handler, reader
      test/                                 4 test executables
  providers/openai/                plugin over the Responses adapter
  providers/deepseek/              plugin over the Chat Completions adapter
  example/                         manual, live-API demos
  test/                            module-level tests (contract, plugins, ABI hygiene)
```

## The layer is visible in the include path

A consumer writes the layer's name:

```cpp
#include "llm/compat/chat_completions/model.hpp"
#include "llm/compat/responses/reader.hpp"
```

That is a deliberate difference from the two groupings this tree already has,
and worth stating because it looks inconsistent at a glance: `toolsets/` in
`tools/intrinsic/toolsets/process/` and `providers/` in
`llm/providers/deepseek/` are dropped from the logical path (`tools/intrinsic/…`,
`llm/deepseek/…`), while `compat/` is not. Those two are *storage* groupings —
where a submodule happens to sit in a collection — and naming them in an include
path would make a header's identity depend on the shelf it stands on. `compat/`
is not that: it is a semantic layer with its own dependency rules, its own build
targets and its own README, and putting it in the path makes two things true at
once:

- reading a single `#include` line says which protocol, and which layer of the
  module, that translation unit is speaking to;
- `grep -rn '#include "llm/compat/'` lists every consumer of the compatibility
  layer in the tree. Before this move that list could only be recovered from the
  build files, and three of its entries were missing there (see the last
  section).

## What did NOT change

The extraction moved files and include paths; it did not rename the code:

- **namespaces**: `llm::chat_completions`, `llm::responses`;
- **targets and SONAMEs**: `llm_chat_completions` and `llm_responses` still build
  `libllm_chat_completions.so` / `libllm_responses.so`, so the ABI story in
  `docs/abi-context.md` (shared adapters, one authoritative typeinfo per process,
  plugins bound by `DT_NEEDED`) is untouched;
- **test names**: the nine adapter test executables keep their ctest names;
- **the release install set**, re-checked at configure time: the same target list
  as before the move.

## What did change: the dependency is now explicit, and the path says so

An adapter header no longer arrives through `llm_iface`'s include directory, so
a translation unit that includes one must link the adapter that publishes it:

```cmake
target_link_libraries(<host> PRIVATE llm_iface llm_chat_completions)
```

Three targets had been relying on the ambient include directory and now say so:
`llm_deepseek_chat` (subscribes to the adapter's reasoning events),
`test_deepseek_plugin` (same header, module-level test), and `llm_deepseek`
itself, whose public `dialect.hpp` includes the adapter's dialect header and
therefore declares the link `PUBLIC`. Everything else already named its adapter.

That is the intended effect of the move rather than a cost of it: which protocol
a component speaks is now visible in its build file and in its include lines,
instead of being inherited from the module-wide include path.
