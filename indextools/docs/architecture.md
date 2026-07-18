# indextools — Architecture & Design (Phase 1)

This document describes the internal architecture of the indextools server as
built in Phase 1: a single-process, fully asynchronous JSON tool server speaking
NDJSON over stdio. It complements the contract-facing docs
([protocol.md](protocol.md), [commands.md](commands.md), [schemas.md](schemas.md),
[server.md](server.md)) by explaining *how* the implementation is layered and
*why*, and it closes with the open question about a CLI client.

Everything here reflects what is committed and tested, not aspiration. Where a
documented feature is parsed-but-not-enforced, it says so.

---

## 1. Layering at a glance

A request flows top-to-bottom; each layer has one job and hands a narrower
problem to the next.

```
 stdin (NDJSON)                                            stdout (NDJSON)
      │                                                          ▲
      ▼                                                          │
 ┌─────────────────────────────────────────────────────────────────────┐
 │ Transport            stdio_server.{hpp,cpp}                           │
 │   serve_ndjson()  read line → process_line() → write line            │
 │   process_line()  json::parse → parse_envelope → dispatch → to_json  │
 └─────────────────────────────────────────────────────────────────────┘
      │ CommandRequest                                          │ CommandResponse
      ▼                                                          │
 ┌─────────────────────────────────────────────────────────────────────┐
 │ Routing              command_dispatcher.{hpp,cpp}                    │
 │   method-known check · capability gates · param validation          │
 │   server.* handled here · session.open/close → manager              │
 │   everything else → resolve token → session.dispatch()              │
 └─────────────────────────────────────────────────────────────────────┘
      │                                                          │
      ▼                                                          │
 ┌─────────────────────────────────────────────────────────────────────┐
 │ Session store        session_manager.{hpp,cpp}                       │
 │   token → ToolSession · lazy create · open/close lifecycle          │
 └─────────────────────────────────────────────────────────────────────┘
      │ ToolSession (per token, on its own strand)               │
      ▼                                                          │
 ┌─────────────────────────────────────────────────────────────────────┐
 │ Session              session.{hpp,cpp}                               │
 │   routes session.* / file.* / edit.* / binary.* / search.* / proc.* │
 │   owns the services below; all state mutated on one strand          │
 └─────────────────────────────────────────────────────────────────────┘
      │                                                          │
      ▼                                                          │
 ┌───────────────┬───────────────┬───────────────┬─────────────────────┐
 │ FileService   │ BinaryService │ SearchInterface│ SubProcessManager   │
 │ read/edit     │ inline/ref    │ CacheSystem +  │ spawn/collect/...    │
 │ path-confined │ base64 chunks │ plugins        │ (Boost.Process v2)   │
 └───────────────┴───────────────┴───────────────┴─────────────────────┘
                          │ path_policy.hpp confines every filesystem path
                          ▼
                    schema.hpp  (DisplayBlock[] / ProcessReport[] builders)
```

The dividing principle: **structural concerns move up, semantic concerns move
down.** The transport knows framing but not methods; the dispatcher knows
methods and gates but not file semantics; a service knows its own domain but
neither the wire nor the session store.

---

## 2. Data types that cross layers

- **`CommandRequest`** (`command.hpp`) — `{id, token, method, params}`, produced
  by `parse_envelope` from raw JSON. `id` stays raw JSON so string/number/null
  ids round-trip unchanged.
- **`CommandResponse`** (`command.hpp`) — `{id, ok, result | error}`, serialised
  by `to_json()`. Built with `make_ok` / `make_error`.
- **`CommandError`** (`command.hpp`) — `{code, message, details?}` where `code`
  is one of the nine closed `error_code::*` constants.
- **Service results** — every service returns `std::variant<nlohmann::json,
  CommandError>` (`FileResult`, `BinaryResult`, `ManagerResult`). The session
  turns that variant into a `CommandResponse` echoing the request id via a
  single `result_to_response` helper, so no service ever touches an envelope.

The two wire *result* shapes (`DisplayBlock[]`, `ProcessReport[]`) are defined
once in `schema.hpp` with builders (`MetaBuilder`, `TextBody`, `text_block`,
`content_block`, `ProcessReport`). Producers never hand-assemble that JSON.

---

## 3. Concurrency model

Everything runs on one `boost::asio::io_context`. There is no thread pool in
Phase 1; the async design is about *not blocking on I/O* (subprocess pipes,
future sockets), not about parallel CPU.

- **Strand-per-owner.** Each `ToolSession`, each `SubProcessManager` (and each of
  its instances), the `CacheSystem`, and the `SessionManager` own a
  `strand<any_io_executor>`. Every public coroutine begins with
  `co_await dispatch(strand, use_awaitable)`, so all of that object's state is
  mutated serially without explicit locks. The strand *is* the lock.
- **Search fan-out is deliberately off the session strand.** `SearchInterface`
  is constructed on the session's *base* executor, not its strand, so
  `launch_search` can fan out concurrent per-file analysis; the `CacheSystem`
  keeps its own strand to protect the analyzer map. Putting search on the
  session strand would have serialised the fan-out it exists to parallelise.
- **`BinaryService` uses a plain `std::mutex`**, not a strand: its critical
  sections (resource-table insert/erase) are tiny and synchronous, and it has no
  coroutines of its own. The session strand already serialises calls into it;
  the mutex is belt-and-suspenders for the resource map.

---

## 4. Layer-by-layer

### 4.1 Transport — `stdio_server.{hpp,cpp}`

`process_line(dispatcher, line)` is the pure per-line pipeline and is where the
protocol's robustness rules live: a blank/whitespace line is skipped
(`nullopt`); a line that is not JSON becomes a `parse_error` response with
`id: null` **without dropping the connection**; a structurally invalid envelope
becomes an error echoing whatever id could be recovered.

`serve_ndjson(dispatcher, in, out)` is a template over any Asio
`AsyncReadStream` + `AsyncWriteStream`. It `async_read_until('\n')`, runs
`process_line`, writes the response line, and stops on EOF or once
`dispatcher.shutdown_requested()` latches. Being a template is what lets the
integration test drive the exact same loop over an Asio pipe pair instead of
real fds.

`run_stdio_server` binds the loop to `dup()`-ed `STDIN_FILENO`/`STDOUT_FILENO`
wrapped in `posix::stream_descriptor`.

### 4.2 Routing — `command_dispatcher.{hpp,cpp}`

Order matters and is fixed:

1. **Capability hiding.** `server.shutdown` (without `--allow-shutdown`) and any
   `process.*` (without `--allow-process`) are made to look like
   `unknown_method` — an unauthorised client cannot even tell they exist.
2. **Method-known check** → `unknown_method`.
3. **Param validation** (`validate_params`) → `invalid_params`.
4. **Route.** `server.info` / `server.shutdown` handled here (shutdown also
   `close_all()`s every session). `session.open` / `session.close` go to the
   manager. Everything else: resolve the token to a session (creating it
   lazily) and delegate to `ToolSession::dispatch`.

`server.info`'s `supported_extensions` / `plugins` come from the process-wide
`LangPluginManager`.

### 4.3 Session store — `session_manager.{hpp,cpp}`

Owns `token → shared_ptr<ToolSession>` behind its own strand and is the only
place sessions are created or destroyed.

- **Lazy** (`get_or_create`): any request with a new token mints a session with
  the server default config; an empty token maps to `--default-token`.
- **`session.open`**: binds an explicit `root_path` / `context_lines` /
  `num_tasks`. Re-opening the same token with the **same** (canonicalised) root
  is idempotent (`created:false`); a **different** root is refused with
  `invalid_params` (client must close first); a root that is not an existing
  directory is `invalid_params`.
- **`session.close`**: `co_await session->shutdown()` (terminates the session's
  processes), unbinds the token, reports `terminated_processes`. Unknown token
  is a non-error `closed:false`.

### 4.4 Session — `session.{hpp,cpp}`

`ToolSession` holds one token's `SessionConfig` and the four services, all on its
strand. It routes the session-scoped families and owns two cross-cutting rules:

- **Read caps.** `file.read_lines` / `file.read_bytes` clamp `max_lines` /
  `max_bytes` to the server-wide `--max-read-lines` / `--max-read-bytes`.
- **`set_config`** rebuilds `FileService` when `context_lines` changes (that
  value is baked into the service) but leaves `BinaryService` — and its live
  resource handles — untouched.

`SessionConfig` (per-session, mutable) and `ServerCapabilities` (server-wide,
fixed at startup, high-risk switches default off) are the two plain value types
threaded through construction.

### 4.5 Services

- **`FileService`** (`file_service.{hpp,cpp}`) wraps `viewer.hpp` +
  `editor.hpp`. Reads return `DisplayBlock[]`. Edits **default to dry-run**
  (compute + return diff, no write); a write happens only when `apply:true`
  **and** `--allow-write`, and only after re-reading the file and confirming it
  still matches the snapshot the edit was computed from (a concurrent change is
  refused with `edit_conflict`, never clobbered). `str_replace` maps a
  non-unique / absent match to `edit_conflict` with `details.matches`.
- **`BinaryService`** (`binary_service.{hpp,cpp}`) implements the two-form
  contract: inline base64 `binary` block when the file is within
  `--inline-binary-limit` (unless `force_ref`), else a `binary_ref` handle whose
  bytes are pulled with `read_chunk` until `eof` and dropped with `release`. The
  resource table is per-session, so handles are isolated. Self-contained RFC 4648
  base64 and extension-based MIME guessing live here.
- **`SearchInterface`** (`cache_system.hpp`, pre-existing) is driven for
  `search.*`; a `root_path` override is still confined, `glob` defaults to
  `**/*`, and a per-file analyzer throw is swallowed into `internal_error`
  rather than failing the batch.
- **`SubProcessManager`** (`subprocess.hpp`, pre-existing) backs `process.*`,
  gated by `--allow-process`. `spawn` honours `--exec-allowlist`; id-targeted
  methods return `not_found` for an unknown id; the manager's query methods
  already emit `ProcessReport[]`.

### 4.6 Path confinement — `path_policy.{hpp,cpp}`

`resolve_within_root(root, user_path)` is the one choke point every
filesystem-touching command passes through. It joins a relative path onto the
root, `weakly_canonical`-ises both (resolving `..` and symlinks as far as they
exist on disk — so it also works for a not-yet-created target), then confirms
containment **component-by-component** (so `/srv/rooted` is not mistaken for a
child of `/srv/root`). Any escape — `..`, an absolute path outside, a symlink
pointing out — becomes `path_denied`. It never throws.

### 4.7 Startup — `server_config.{hpp,cpp}` + `main.cpp`

`parse_server_config` turns argv into a `ServerConfig` (mode + `SessionConfig`
defaults + `ServerCapabilities` + plugin dir + diagnostics). `--help`/`--version`
exit 0; a bad `--root` or option error exits non-zero. `main.cpp` wires
argv → plugin discovery → `SessionManager` → `CommandDispatcher` →
`run_stdio_server`; banner and diagnostics go to **stderr**, protocol responses
only to **stdout**; exit 0 on clean stop (EOF or `server.shutdown`).

---

## 5. Error model

Nine closed codes (`command.hpp` / [protocol.md](protocol.md) §4):
`parse_error`, `invalid_request`, `unknown_method`, `invalid_params`,
`path_denied`, `not_found`, `edit_conflict`, `io_error`, `internal_error`.

Notable mappings chosen in this implementation:

- Capability-off for a gated method → `unknown_method` (not a distinct
  "forbidden" code): the method is hidden, not merely refused.
- `apply:true` without `--allow-write` → `path_denied` (the write itself is what
  is denied; the diff is still returned on the dry-run path).
- Non-unique/absent `str_replace`, and the concurrent-change refusal →
  `edit_conflict`.
- A search `root_path` that escapes or is not a directory → `path_denied`.

---

## 6. Testing strategy

One Boost.Test binary per unit, plus one integration binary:

| Test | Covers |
|---|---|
| `test_command` | envelope parse / build / serialise |
| `test_command_schema` | method registry + per-command param validation |
| `test_path_policy` | containment + resolve (escapes, symlink, sibling-prefix) |
| `test_file_service` | read shapes, dry-run vs apply, write gate, edit_conflict |
| `test_binary_service` | base64 vectors, inline vs ref, chunk-to-eof, release |
| `test_dispatcher` | end-to-end on an io_context: gates, validation, routing, search, process, lifecycle |
| `test_stdio_server` | serve_ndjson over Asio pipe pairs: round-trip, parse_error recovery, shutdown + EOF |

The async tests drive real coroutines on an `io_context` (mirroring
`test_subprocess`). Process tests use a persistent-session coroutine runner so a
child can exit and be reaped between dispatches. The transport test uses
`connect_pipe` so no real fds are involved.

Build/run loop: `cmake -S . -B build` (re-globs tests) → `cmake --build build` →
`cd build && ctest`. Runtime binaries land in `bin/` (not `build/bin/`).

---

## 7. Known limitations (parsed but not enforced)

- **`--tcp`** is parsed; the server exits with code 2 (not implemented).
- **`--session-idle-timeout`** is parsed and stored but no idle reaper runs;
  sessions are reclaimed only by `session.close` or `server.shutdown`.
- **`--log-level`** gates only the startup banner; there is no structured logger
  yet (diagnostics are ad-hoc stderr writes).
- **`cached_files`** in `session.info` is always 0 — `CacheSystem` exposes no
  size accessor and adding one was out of scope.

---

## 8. The client question (open)

The original plan included a C++ CLI that turns argv into JSON and talks to the
server, "mainly for CLI-agent compatibility and debugging." That work is **not
started**, deliberately, because its shape depends on a transport decision we
have not made. The considerations, recorded so the audit can settle them:

**The determining constraint.** Under stdio, session state lives only inside one
server *process* (1 client : 1 process). So:

- A **one-shot CLI** (`spawn server → send one → read one → exit`) is trivial but
  gets a **cold cache and no session** every call — it throws away the session /
  cache machinery for any repeated-query workflow.
- A **one-shot CLI that reuses a session** is *impossible over stdio*. It
  requires a resident daemon + Unix-domain (or TCP) socket — i.e. the "network
  layer" from the original roadmap, which does not exist yet.
- A **REPL / long-lived CLI** that owns the server as a child process can hold a
  session, but is an interactive process, not a Unix-style one-shot command.

**Three separable jobs inside "the CLI".**

1. *Envelope construction* (argv → JSON) — the highest-coupling, highest-
   maintenance part: every new method needs subcommand/flag mapping kept in sync
   with `command_schema`. This is the only genuinely heavy piece.
2. *Rendering* (JSON → text) — largely already exists: `utils.hpp`'s
   `json_to_readable_text()` renders `DisplayBlock[]` / `ProcessReport[]`. A
   `--render text` switch on the *server* could push this server-side and shrink
   the client to almost nothing.
3. *Transport / session* — the daemon-vs-stdio fork above.

**Recommendation.**

- Do **not** build a full argv-grammar compiled client before the transport is
  chosen; in a one-shot agent model it would ship half-done (cold cache).
- For debugging now, a thin shell wrapper feeding `bin/indextools --stdio` plus
  `jq` is near-zero cost; optionally add a server-side `--render text` so even
  that needs no client.
- If a client is built, prefer a **thin passthrough + renderer** (reads JSON or
  `raw '{...}'` from stdin, forwards, renders the response) over argv-grammar
  mapping — it stays decoupled from the command set, so new methods need zero
  client changes. Add argv sugar (`read foo.py --lines 0:40`) only once the
  command set is stable and a human is actually typing it.

**Questions the audit should answer** (they largely determine the client's
form):

1. Primary consumer — you debugging, or an autonomous CLI agent invoking tools
   in a shell?
2. If an agent: one-shot subprocess calls, or can it hold a long-lived
   connection/session? (Decides whether a daemon is needed.)
3. Is a network/daemon transport (Unix socket) on the near-term roadmap? If yes,
   the client should be designed as a thin socket client; if it's stdio-only for
   a while, only the debugging wrapper is worth building now.

