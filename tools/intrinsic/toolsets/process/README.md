# process — intrinsic toolset

Six tools that let a model run child processes: start one, check on it, read
what it printed, feed it input, wait for it, kill it. A process outlives the
call that started it, so each one gets a **session id** the model uses to come
back to it in later turns.

Built on the package's shared core (`tools_intrinsic`, see
[`../../README.md`](../../README.md)), which carries the argument reading, the
JSON result shape and the confirmation routing; this directory is only the
process domain. The manager underneath is `process/`'s `ProcessHandle`.

- [The model's view](#the-models-view) — the six tools, their schemas and
  results
- [Worked examples](#worked-examples)
- [The host's view](#the-hosts-view) — wiring, shutdown, internals

---

## The model's view

### How the tools fit together

`spawn_process` waits a short while (5 seconds by default) for the program to
finish. An ordinary command — `ls`, `grep`, a quick build step — finishes
inside that window, so its exit code and its whole output come back in the
**same call** that started it: no separate `run_command`, because
`spawn_process` already is one. A program still running when the window closes
keeps running in the background instead, and the result carries a `session_id`
to come back to it with.

```
spawn_process ──► finished in time? ──► yes: exit code + whole output, done
                                    │
                                    └─► no: session_id ──┬──► wait_process        finish and collect
                                                         ├──► poll_processes      what changed since last time
                                                         ├──► read_process_output  incremental output
                                                         ├──► write_process_input  feed its stdin
                                                         └──► kill_process         end it
```

**Output reads are incremental by default.** Each session remembers how much
of its output has already been handed over, so `read_process_output` and
`poll_processes` return only what is *new* — a poll loop does not re-read the
same text every turn. Pass `full: true` for the whole capture; a full read
leaves the incremental position alone, so it never steals bytes from a poll
loop.

**Sessions have to be let go.** A finished process keeps its session (and its
output) until it is released, so the model can still read it. Release with
`release` on a read or wait, or `release_exited` on a poll. At most **32**
sessions may be retained at once — an exited-but-unreleased session still counts
— and spawn refuses past that, which is a failure the model reads and can act on
by releasing finished sessions.

**Session ids are never reused.** `proc_3` names one process for the life of the
host, and after that session is released the number is spent: the next spawn
gets a fresh one. A model's older context still says "proc_3 is the build I
started three turns ago", and an id that came back around would make
`kill_process(proc_3)` end a process that model never saw.

**There is no shell.** The executable runs directly, so `|`, `>`, `*` and `&&`
reach it as literal arguments. A model that wants a pipeline asks for
`sh -c '...'` explicitly.

### Which calls need confirmation

| | tools | why |
| --- | --- | --- |
| **Asks first** | `spawn_process`, `write_process_input`, `kill_process` | they change state outside this process — running a program, feeding it, ending it |
| **Runs unattended** | `poll_processes`, `read_process_output`, `wait_process` | looking at what is already running changes nothing |

A confirmation that nobody answers is a refusal, so an unattended host runs the
observing calls and refuses the rest.

### Which calls may run beside their neighbours

The host schedules a turn's calls in one batch, and each call's settled
arguments decide whether it may overlap the others. The rule is "does this call
disturb what a neighbour in the same batch could observe":

| call | runs beside others | runs alone |
| --- | --- | --- |
| `spawn_process` | — | always (it changes the machine) |
| `kill_process` | — | always |
| `write_process_input` | — | always (two writes to one child do not commute) |
| `poll_processes` | `include_output: false, release_exited: false` | any poll that reads output or reaps |
| `read_process_output` | `full: true, release: false` | the delta read (`full: false`), or any `release` |
| `wait_process` | `release: false` | `release: true` |

A delta read advances that session's position, so two of them in one batch are
order-dependent whichever way they interleave; a `release` removes the session a
neighbour may be addressing. Both are therefore serial — the batch gives them
the executor to itself, and the rest of the batch waits. Calls that run alone go
**first**, in call order, so a poll beside a spawn already sees the session that
spawn created.

---

### `spawn_process`

Run a program. Waits up to `expected_runtime_milliseconds` for it to finish; if
it does, the result carries its exit code and its whole output. If it does
not, it keeps running and the result carries a `session_id` instead.

```jsonc
{
  "executable": "grep",                    // required: PATH name or path
  "arguments": ["-rn", "TODO", "src/"],    // one element per argument, verbatim
  "description": "find TODOs",             // label echoed in every later report
  "working_directory": "/home/me/project", // defaults to the host's own cwd; "" is refused
  "environment": ["LANG=C"],               // "KEY=VALUE", merged over the inherited env
  "inherit_environment": true,             // default true
  "expected_runtime_milliseconds": 5000    // default 5000; 0 returns a session id at once
}
```

Only `executable` is required. Arguments are passed **verbatim** — do not quote
or escape them, and do not put them all in one string. `working_directory: ""`
is refused rather than read as "not given": a child cannot be started in the
empty path, and silently inheriting the host's directory would run the command
somewhere the model did not ask for.

**A quick command finishes inside the window** and comes back complete:

```jsonc
{
  "session_id": "proc_1",
  "executable": "grep",
  "arguments": ["-rn", "TODO", "src/"],
  "description": "find TODOs",
  "pid": 48231,
  "finished": true,
  "output_complete": true,
  "state": "exited",
  "exit_code": 0,
  "running_milliseconds": 8,
  "stdout_text": "src/main.cpp:12: // TODO\n",
  "stderr_text": "",
  "stdout_truncated": false,
  "stderr_truncated": false,
  "hint": "the process finished; its output is above. Call read_process_output with release to forget the session when done with it"
}
```

The session is **kept**, not reaped, even though it already finished: its
output stays readable, and the model releases it when done (`release: true` on
a later `read_process_output`, or `release_exited` on a poll).

**`finished` and `output_complete` are two different facts**, and the second is
not implied by the first. `finished` is about the child: it exited inside the
window, and its exit code is readable. `output_complete` is about its pipes: the
capture finished too, so the text above is everything the process printed. They
part company when a command starts something else that inherits its output and
then exits itself — a launcher, a background job — because the descendant keeps
those pipes open after the child is gone:

```jsonc
{
  "session_id": "proc_2",
  "finished": true,
  "output_complete": false,
  "state": "exited", "exit_code": 0,
  "stdout_text": "…what has arrived so far…",
  "hint": "the process finished, but its output capture is not complete yet: … call wait_process to collect the rest"
}
```

**A program that outlives the window** comes back as just the id and the
in-progress state — no `exit_code`, no output slice:

```jsonc
{
  "session_id": "proc_3",
  "executable": "make",
  "pid": 48244,
  "finished": false,
  "output_complete": false,
  "state": "running",
  "running_milliseconds": 5000,
  "hint": "the process is still running; call poll_processes to check on it, wait_process to wait for it, read_process_output to read its output"
}
```

Raise `expected_runtime_milliseconds` for a command that legitimately needs
longer but is still worth waiting for; pass `0` to skip the wait entirely and
get a `session_id` right away, for a program the model already knows will run
for a while (a server, a watch).

A launch that fails — no such executable, a working directory that is not a
directory, a malformed environment entry — comes back as a failure naming what
went wrong, not as a session.

### `poll_processes`

The state of every session, and what each has printed since the last read.
This is the call for checking on work started earlier.

```jsonc
{
  "session_ids": ["proc_1", "proc_2"],  // omit for all sessions
  "include_output": true,               // default true
  "release_exited": false               // default false
}
```

All three are optional; `{}` reports everything.

```jsonc
{
  "sessions": [
    {
      "session_id": "proc_1",
      "executable": "grep", "arguments": ["-rn", "TODO", "src/"],
      "description": "find TODOs", "pid": 48231,
      "state": "exited",              // "running" | "exited" | "unknown"
      "exit_code": 0,                 // absent while still running
      "running_milliseconds": 412,
      "new_stdout": "src/main.cpp:12: // TODO\n",   // since the last read
      "new_stderr": ""
    }
  ],
  "retained_session_count": 1,
  "released": ["proc_1"]              // only when release_exited actually freed some
}
```

`exit_code` is **absent** while a process runs — an absent exit code is not a
zero one. With `release_exited: true`, output is reported *before* the session
is freed, so nothing is lost. `retained_session_count` counts what the table is
holding, exited-but-unreleased sessions included — the same number the
32-session cap counts, so it is not the number of *running* processes.

### `read_process_output`

Read what one process has printed. Incremental by default, so it can be called
repeatedly while the process runs.

```jsonc
{
  "session_id": "proc_1",  // required
  "stream": "both",        // "stdout" | "stderr" | "both" (default)
  "full": false,           // default false: only what is new
  "release": false         // default false; only applies once exited
}
```

```jsonc
{
  "session_id": "proc_1",
  "stream": "both",
  "full": false,
  "state": "running", "pid": 48231, "executable": "grep", /* …session fields… */
  "stdout_text": "src/main.cpp:12: // TODO\n",
  "stdout_truncated": false,
  "stdout_bytes_read": 34,     // total handed over so far, across all reads
  "stderr_text": "",
  "stderr_truncated": false,
  "stderr_bytes_read": 0,
  "released": true             // present only when release was asked for
}
```

`*_truncated` means the process printed more than the capture limit (4 MiB
shared between the two streams) and the text stops there. `released` reports
what actually happened, not what was asked: a running process is never
released, so it comes back `false`.

### `write_process_input`

Send text to a running process's standard input.

```jsonc
{
  "session_id": "proc_1",   // required
  "input": "some text\n",   // verbatim; include "\n" if it reads by lines
  "close_input": false      // default false
}
```

Either `input` or `close_input` must say something — a call that sends nothing
and closes nothing is refused as a mistake.

```jsonc
{
  "session_id": "proc_1",
  "bytes_queued": 10,
  "input_closed": false,
  "state": "running",
  "note": "the text is queued for the process's standard input; the process may not have read it yet",
  "warning": "the process has already exited, so the input was discarded"  // only if it had
}
```

`bytes_queued`, not "delivered": the write is handed to a background pump and
this returns before the process reads it. To confirm it was consumed, read the
output back. Many programs read until end-of-input — for those, send the text
and then `close_input: true`, or nothing happens.

### `wait_process`

Wait for a process to finish and report how it ended, with everything it
printed.

```jsonc
{
  "session_id": "proc_1",           // required
  "timeout_milliseconds": 30000,    // default 30000; 0 waits indefinitely
  "release": false                  // default false
}
```

```jsonc
{
  "session_id": "proc_1",
  "executable": "grep", "pid": 48231, /* …session fields… */
  "state": "exited",
  "exit_code": 0,
  "running_milliseconds": 412,
  "exited": true,
  "output_complete": true,
  "timed_out": false,
  "stdout_text": "src/main.cpp:12: // TODO\n",   // the FULL capture
  "stderr_text": "",
  "stdout_truncated": false,
  "stderr_truncated": false
}
```

The three fields say which of the two things the wait was waiting for it
reached: `exited` — the child is gone, its exit code readable; `output_complete`
— its output is all here, both pipes closed; `timed_out` — the wait ended
without reaching both, which is the negation of `exited && output_complete`
rather than of `exited` alone, so the three can never disagree. A child that
exits while something it started still holds its stdout open ends the wait with
`exited: true, output_complete: false, timed_out: true` — a real state, and the
reason the distinction exists; the result then carries a hint saying so, and
waiting again collects the rest.

**A timeout is not an error.** It comes back `"exited": false, "timed_out":
true` with `state` still `"running"`, and the process keeps running — wait
again, read its output, or kill it. `timeout_milliseconds: 0` waits forever,
which hangs the turn on a process that never exits; prefer a real timeout and
wait twice.

Unlike a read, this returns the **whole** capture, with `full = true`: a caller
waiting for a command to finish wants all its output and cannot know what an
earlier poll already consumed. Because a full read is an observation and not a
consumption, a wait running beside a poll loop steals nothing from it.

### `kill_process`

End a running process. The session stays readable afterwards, so its output can
still be collected.

```jsonc
{
  "session_id": "proc_1",  // required
  "graceful": false        // default false: kill outright
}
```

`graceful: true` asks the process to shut down (a signal it may handle, or
ignore); the default ends it immediately and cannot be refused.

```jsonc
{
  "session_id": "proc_1",
  "state": "running",       // may still say running — see below
  "pid": 48231, /* …session fields… */
  "signalled": true,
  "graceful": false,
  "note": "signal sent; call wait_process to confirm the process has ended"
}
```

The signal is sent, but the death is noticed a moment later, so the result may
still report `"running"` — that is not a failed kill. Call `wait_process` to
confirm. A process that had already finished comes back `"signalled": false`
with a note saying so, which is not a failure either.

---

## Worked examples

**Run a command and get its output.** One call — it finishes inside the
default window:

```jsonc
spawn_process { "executable": "ls", "arguments": ["-la", "/tmp"], "description": "list /tmp" }
   → { "session_id": "proc_1", "finished": true, "state": "exited", "exit_code": 0,
       "stdout_text": "total 48\n…" }
```

The session is kept in case its output is wanted again; release it once done:

```jsonc
read_process_output { "session_id": "proc_1", "release": true }
   → { "stdout_text": "", "released": true }   // full text already returned by spawn
```

**Watch a long build.** Spawn with a short window (or `0`) so it becomes a
session at once, then poll as often as needed:

```jsonc
spawn_process   { "executable": "make", "arguments": ["-j4"], "description": "build",
                  "expected_runtime_milliseconds": 0 }
   → { "session_id": "proc_2", "finished": false, "state": "running", … }

poll_processes  { "session_ids": ["proc_2"] }
   → { "sessions": [{ "state": "running", "new_stdout": "[ 10%] Building…\n" }] }

poll_processes  { "session_ids": ["proc_2"] }          // only what is NEW
   → { "sessions": [{ "state": "running", "new_stdout": "[ 45%] Building…\n" }] }

poll_processes  { "session_ids": ["proc_2"], "release_exited": true }
   → { "sessions": [{ "state": "exited", "exit_code": 0, "new_stdout": "[100%] Built\n" }],
       "released": ["proc_2"] }
```

**Feed a program on stdin.** `cat` reads until end-of-input, so it has to
become a session before there is anything to write to — pass `0` so the spawn
does not wait for a program that is waiting right back:

```jsonc
spawn_process         { "executable": "cat", "description": "echo back",
                        "expected_runtime_milliseconds": 0 }
   → { "session_id": "proc_3", "finished": false, … }

write_process_input  { "session_id": "proc_3", "input": "hello\n", "close_input": true }
   → { "bytes_queued": 6, "input_closed": true }

wait_process         { "session_id": "proc_3", "release": true }
   → { "exited": true, "exit_code": 0, "stdout_text": "hello\n" }
```

**A pipeline needs an explicit shell:**

```jsonc
spawn_process { "executable": "sh", "arguments": ["-c", "ls /tmp | wc -l"], "description": "count files" }
```

Without `sh -c`, `|` and `wc` would reach `ls` as literal arguments.

**Stop something that is taking too long:**

```jsonc
wait_process { "session_id": "proc_4", "timeout_milliseconds": 5000 }
   → { "exited": false, "timed_out": true, "state": "running" }   // not an error

kill_process { "session_id": "proc_4" }
   → { "signalled": true, "note": "signal sent; call wait_process to confirm…" }

read_process_output { "session_id": "proc_4", "full": true, "release": true }
   → { "stdout_text": "…everything it printed before it died…", "released": true }
```

---

## The host's view

### Wiring it up

```cpp
auto store = std::make_shared<tools::intrinsic::ProcessSessionStore>(executor);
registry.add(std::make_shared<tools::intrinsic::ProcessToolSet>(store));
```

The store is a `shared_ptr` because a host usually holds it too — to terminate
everything at shutdown, or to show a live process list in a UI. An optional
second argument to the set is an `AsyncEventBus*`: `nullptr` (the default) uses
the process-wide bus, so a confirmer in another module can answer; passing one
keeps a component's confirmations to itself.

Since the state-changing tools declare `RequireConfirm`, a host that wants them
to run at all must subscribe a handler to `InvokeConfirmEvent` — with none,
they are refused (silence is not consent). What the handler is shown is the
**settled** call: the query with every default written in, exactly the one that
will run and exactly the one the record carries back. `ProcessSessionStore`'s
constructor also takes a `max_sessions` cap, default `kDefaultMaxSessions`
(32), which counts retained sessions rather than live processes.

### Shutdown is `terminate_all()`, not the destructor

Each handle's await task keeps its own handle alive until the child's terminal
state is observed, so dropping the table is not the last reference and does not
stop the children. Killing one is a coroutine, and a destructor has no executor
to run it on. So a host awaits `terminate_all()` **while its context still
runs**. The destructor is a last-resort tail: it signals the recorded pids
synchronously and logs loudly.

Drop the table **after** the context is quiesced — stopped, with its threads
joined. The tail's signal is a plain `::kill` on a pid recorded at spawn, so it
needs no executor and works either way; what it cannot survive is being freed
while the context is still running, because the table's strand shares
refcounted state with the operations queued on it. That is a race in the
executor's refcount, and ThreadSanitizer reports it (the suites here quiesce
first, for exactly that reason).

### The three headers

- **`process/session_store.hpp`** — the session table. Mints readable ids
  (`proc_1`, `proc_2`, … monotonically, never reusing one), gives each child its
  own strand, keeps the per-stream read cursors that make "what is new since I
  last looked" answerable, and reaps a session once its child is observed
  terminal. Two levels of strand: the store's own serialises the table, a
  session's serialises its handle and cursors — so nothing that leaves the class
  is a reference into it, only values (a snapshot, a slice of output, a bool).
  `release()` is the one method that hops twice, reading the child's terminal
  state on the session's strand and removing the entry on the store's, because
  a lifetime decision read off the wrong strand is a data race that passes every
  single-runner test. It owns `ProcessHandle`'s lifecycle contract in full:
  start the io tasks, then drive the handle to a terminal observation with a
  detached await task, and never block a spawn on the child.
- **`process/tools.hpp`** — the six `ToolInterface` implementations. Each
  checks its arguments in `ensure_arguments()` and writes the defaults into the
  query there (so the security check and the human confirmation see settled
  arguments), and answers with a JSON object in a text part. `InvokeType` is
  decided from those settled arguments, not from the tool's name: a read that
  consumes a cursor, a poll that reaps and a wait that releases are all
  `SerialWrite`, because a neighbour in the same batch can observe their order.
- **`process/toolset.hpp`** — the `ProcessToolSet` a host registers. Its name,
  its six tools and the store they share; the catalogue, the routing and the
  build/release lifecycle come from `IntrinsicToolSet`, and
  `prepare()` / `execute()` stay as `ToolSet` defines them, since those carry
  the invocation layer's checkpoint sequence and failure contracts.

### Deliberately absent

No separate `run_command` — `spawn_process` already is one.
`ProcessHandle::await_initial_execution()` (`process/`) was designed for
exactly this split: wait out an initial grace window, answer with the whole
result if the child finished inside it, or detach and hand back a live session
if it did not. The tool honours that window instead of disabling it, which is
what turns a program's own runtime into the signal, rather than putting that
decision on the model.

No shell parsing — there is no shell on the other side, and a model that wants
a pipeline says so by spawning `sh -c`. No reap tool — reaping is a flag on the
reading tools, because the moment a dead child's last output has been read is
exactly when its session becomes garbage, and a separate call would be a step
to forget.

### Supporting changes in the layers below

All three landed with this toolset and are used by it:

- `LaunchSpec::working_directory` (dataclass; applied by Boost.Process v2 as a
  `chdir` in the child, checked before the launch so a bad path names itself at
  `Stage::Spawn`).
- `ProcessHandle::terminate()` / `request_exit()` — on-demand SIGKILL / SIGTERM
  that send the signal only, leaving the terminal observation to the await task
  that owns it.
- `ProcessHandle::output_drained()` — whether both output pipes have hit EOF.
  **Not** implied by `exited()`: the await task records the terminal status as
  soon as it observes the child, while the readers may still be draining what
  is in the pipe buffers, or may be unable to finish at all because a descendant
  inherited the pipes and is still holding them. `wait_for_exit()` waits for the
  pair and reports both facts, and `spawn_process` reports `finished` and
  `output_complete` separately for the same reason. Pinned by a repeated test
  that runs the spawn and the wait in one coroutine — the usual per-call round
  trip adds enough latency to hide the race — and by a regression test whose
  direct child exits while a background descendant keeps its stdout open.
- `ProcessHandle::exited()` is now a **latch** (`std::atomic<bool>`, written
  once by the await task that observes the child, never unwritten) rather than a
  plain read of strand-owned state. That is what lets a destructor ask the one
  question it must — "was this child already observed, and therefore already
  reaped?" — without a strand to hop to: signalling a pid whose child was reaped
  can kill somebody else's process, and the answer has to be right rather than
  lucky. Everything the terminal state is read *for* (the status, the captured
  output, whether the pipes are drained) is still strand-side, and the store
  still hops for all of it.

Built SHARED per `docs/abi-context.md`, like the core it links: hosts and
dlopened plugins may both subclass or catch these types, so their typeinfo must
resolve to one authoritative copy per process.

### Tests

`test_session_store` — ids (monotonic, never reused), the delta/full read
distinction and its per-stream cursors, the difference between a finished child
and a finished capture, the deadline override, the retained-session cap, waiting
with and without a deadline, the refusal to release a live child (and a
concurrent release of one session having exactly one winner), both shutdown
paths — the last of them on a context with three worker threads, where a
strand mistake has somewhere to show up.

`test_tools` — each tool's result, every malformed argument's `ArgumentParse`
failure, the `Invoke` failure for a session that is gone, the type/security each
tool declares for a given settled call (asserted after settling, so a
`write_attributes` that never ran cannot pass), the defaults materialized into
the settled query, the unconfirmed-call refusal, and a whole turn through a
`ToolRegistry` batch.

`test_registry_e2e` — the composition the agent loop uses, at the registry
boundary: `ToolRegistry` + `ProcessToolSet` + `ProcessSessionStore` + its own
event bus + real children, on a context with three worker threads and with every
call going through `ToolRegistry::execute`. Mixed batches (serial first, then
the parallel ones), the settled query and the confirmation question compared
against the record, the scheduling rules measured with a recording probe tool
(serial calls never overlap; read-only and parallel-write calls do), and a
50-turn repetition that gives an ordering regression somewhere to show up.

All three drive real children — the same harmless coreutils `process/`'s own
suite uses — on a context that runs continuously, the way a host does.
