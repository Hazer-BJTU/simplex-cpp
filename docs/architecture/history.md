# Project history

Simplex began as the C++ execution side of a Python agent harness and became a
complete native worker. The layering still shows that origin: the conversation
contracts, the tool registry, and the agent loop were ported one module at a
time, and only then did the worker own a session end to end.

## Before the worker

The first commits brought in `indextools`, a single-process JSON tool server
speaking NDJSON over stdio: code retrieval backed by tree-sitter language
plugins, a session cache, editor and viewer commands, plus shell and subprocess
execution. Its host was the Python harness, which owned the conversation, the
model calls, and the agent loop — the C++ side only answered commands. The
`versioning` and `logging` modules were split out at this point so the tool code
could be linked as a library instead of run as a program.

The `dataclass` module carries that harness's data contracts into C++:
conversation records (`Content`, `InvokeQuery`, `InvokeReturn`, `MessageItem`),
loop progress (`AgentLoopStep`, `UserLoopStep`), the tool registration record
(`Invocable`), the session container (`AgentInputState`), prompt templates, and
endpoint configuration. Those names and shapes still track the Python original.

## The migration

The first pull requests built the half that had stayed in Python:

- `endpoint` and `llm` for provider access — Responses and Chat Completions
  protocol adapters, with DeepSeek and OpenAI as the first provider plugins.
- `extensions` for the plugin ABI, so models, tools, and loop hooks could be
  loaded from a shared runtime.
- `tools` for invocation semantics: checkpoints before and after a call, tool
  sets, and a registry that settles a whole turn's calls.
- `loop` for the cancellable agent loop, its hook registry, and context
  statistics.
- `core` for the worker runtime: one process per session, cancellable
  confirmations, role-based prompts, and session persistence.

`indextools` was removed once the same process owned the conversation; the code
retrieval tool had no remaining role. The Node.js Hub and browser panel followed,
because the bundled terminal shell was a one-to-one server: testing the worker
protocol by hand meant one operator, one session, and typing approval UUIDs.

## After the first release

- **0.1** — release plumbing, an English documentation site, and the first
  rounds of Hub and display-boundary fixes.
- **0.2 – 0.3** — a second provider (Qwen, text and vision), worker installation
  from the Hub, and headless subagent delegation with lifecycle supervision.
- **0.4** — automatic compaction and resume, plus the correctness and
  performance work that made long panel sessions usable.
- **0.5** — the process I/O split with a Linux PTY, musl portable archives, the
  contract-only UI SDK, and startup command bypass policies.
- **1.0** — the persistence backend contract, SQLite as the default backend,
  managed sessions and subagents, and the `simplex_manager` allocation manager.
- **1.1** — archive indexing with trigram search and the `archive_memory`
  toolset, so a worker can search its own history.

## Reading the releases

[CHANGELOG.md](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/CHANGELOG.md)
records every release from `v0.1.0` to `v1.1.0`, grouped as added, changed, and
fixed entries with links to the commit ranges. Releases are cut from `VERSION`
by the tag workflow described in
[CONTRIBUTING.md](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/CONTRIBUTING.md);
dates in the changelog are the tag commits, not npm publication times.
