# Process tools

This intrinsic toolset connects `ToolRegistry` to a session-level
`ProcessSessionStore`. A store owns multiple child lifetimes, stable session IDs
and output cursors. `ProcessHandle` owns each child lifecycle; `process_io` owns
transport, queues, capture and I/O tasks. Native process and PTY mechanisms live
in `utils/subprocess`.

## Tools and approval

| Tool | Purpose | Scheduling / security |
| --- | --- | --- |
| `spawn_process` | Executable and literal argv; explicit environment and auto-release options | SerialWrite / RequireConfirm |
| `run_command` | Shell command, interpreted by bash at conventional paths or otherwise /bin/sh | SerialWrite / RequireConfirm |
| `send_process` | Queue input, request input-end and/or signal a direct child | SerialWrite / RequireConfirm |
| `read_process` | Immediate generic output, optionally full or releasing an exited session | ReadOnly / Trusted |
| `poll_process` | Bounded wait for any selected session, report all selected sessions | ReadOnly / Trusted |

Names, argument schemas and descriptions are loaded from `schemas/*.yaml`;
`schemas/skill.yaml` supplies the injected model guidance. Defaults, including
`tty: false`, are settled before approval and execution. Declared type/security
fields document C++ attributes; editing YAML does not override those attributes.
A rejected launch creates no child or terminal. Respect approval denials.

## Launch

```json
{"executable":"cat","arguments":[],"tty":true,"expected_runtime_milliseconds":0,"auto_release":false}
```

`spawn_process` passes literal argv without shell expansion. Bare executable
names resolve through the assembled child PATH. Explicit paths never fall back
to PATH; relative paths resolve before changing the child's working directory.
Environment entries merge by key, or form the entire environment when
`inherit_environment: false`. An explicit `working_directory` affects only the
child. Invalid paths or unsupported TTY allocation fail visibly.

`run_command` accepts `command`, `working_directory`, `tty` and
`expected_runtime_milliseconds`. It uses the shell's `-c` mode, inherits the
host environment, and always enables initial auto-release. Use shell syntax for
environment changes, or `spawn_process` for explicit environment/retention.
TTY does not add `-i`, launch a login shell or rewrite the command.

Initial waits default to 5000 ms for spawn and 3000 ms for shell commands. At
the store/tool boundary, zero skips waiting; expiration keeps the child alive.
These are observation windows, not execution deadlines. `finished` means child
exit was observed during the initial wait. `output_complete` means capture
ended, including forced stop or failure; check truncation/error metadata too.
Exit may be followed by another bounded output-drain wait. Nonzero child exit
codes remain ordinary results.

Initial auto-release needs **both** child exit and ended capture. When enabled,
output is copied before release. Running/incompletely captured sessions stay
retained; background completion never schedules another auto-release.

## Pipes and terminals

Leave `tty: false` for ordinary commands, builds, tests, searches and structured
output. A long-running command alone does not need a terminal. Enable TTY only
for programs that require a terminal, interactive prompts or `/dev/tty`.

On Linux, `tty: true` supplies a real controlling terminal with all three child
standard descriptors on its slave. Defaults are canonical input, signal
processing enabled, echo off, 80×24 and TERM=dumb. Explicit spawn environment
TERM overrides that default. Programs can change terminal settings themselves.
Host-only resize exists below the tools; dimensions/profiles are not model args.
Unsupported platforms fail instead of silently selecting pipes.

Terminal output merges stdout/stderr and appears once. It may contain ANSI
sequences, CR redraws, prompts and echoed input. This is a raw terminal stream,
not a reconstructed screen. There is no terminal emulator, ANSI stripping,
automatic query response or browser terminal UI.

## Read, poll and result format

```json
{"session_id":"proc_<store-uuid>_1","full":false,"release":false}
```

Launch, read and poll use one `output` block. Pipe stderr is labelled `[stderr]`;
when both pipes have text, both `[stdout]` and `[stderr]` are included. This does
not reconstruct interleaving between pipes. A stdout-only or terminal result
contains plain output. Metadata uses `[[key]]: value` before the data:

```text
[[session_id]]: proc_<store-uuid>_1
[[state]]: exited
[[tty]]: false
[[output_complete]]: true
[[output_truncated]]: false

output (3 bytes):
ok
```

Other metadata includes exit code when known, process identity, elapsed time,
`input_state`, `input_end_requested`, release outcomes and unexpected `io_error`.
Metadata markers are for human readability, not a security boundary or machine
protocol. Historical results keep their original text and remain displayable.

Default reads and polls consume new output and share per-stream raw-byte
cursors. Concurrent readers can split output; callers should not duplicate reads
when they require deterministic grouping. `full: true` and initial launch output
observe without advancing cursors. `output_bytes_read` is the sum of consumed
raw-byte cursors, not rendered text length. Lower-level callers retain access to
stream identities and raw bytes, including a distinct optional terminal stream.

Each process has one retained-byte budget (4 MiB by default; zero is unlimited),
shared across pipe streams or used by the one terminal stream. Exhaustion stops
retention, not drainage. Reads do not replenish capacity. Forced stop/read
failure also marks potentially incomplete capture. A full read cannot recover
truncated bytes.

At the model-text boundary, valid UTF-8 and control bytes are preserved. Invalid
bytes become U+FFFD, one replacement per invalid byte. A valid incomplete UTF-8
suffix remains unread while that stream can still grow; on EOF or truncation it
is replaced. Capture and cursors count original bytes. Full reads apply the same
projection without moving cursors. Mixing raw consuming reads with text reads
can deliberately split a character; the text reader then replaces orphan bytes.

`poll_process` defaults to a 30000 ms wait for ANY selected session to exit with
ended capture, then reports ALL selected sessions. An empty ID list selects all
retained sessions; unknown IDs are ignored. No matches returns immediately.
`timed_out` also covers empty selections and zero-wait checks. Prefer positive
waits over tight polling; remove completed IDs to avoid repeated immediate returns.
`include_output: false` leaves output cursors unchanged.

Release with `read_process.release` or `poll_process.release_exited` after reading
needed output. A running child cannot be released. Exit alone permits explicit
release even if capture is unfinished: shutdown first allows bounded drainage
and joins I/O, then the final capture is converted to text before removal. Any
remaining incomplete UTF-8 suffix becomes U+FFFD and forced closure is marked
truncated. A refused release of a running child does not finalize its suffix.
Status-only polls with release intentionally discard unread output. IDs are never reused and only resolve in their originating store.
Retained exited sessions still count toward the default 32-session capacity.

## Send and input-end

```json
{"session_id":"proc_<store-uuid>_1","input":"hello\n","end_input":true}
```

At least one action is required: nonempty input, `end_input: true`, or signal
`term`/`kill`. Known argument errors are rejected before effects. Order is input,
then input-end, then signal. `input_admission` and `end_input_admission` report
accepted/closed/unsupported; `bytes_queued` counts only accepted input. Acceptance
never proves delivery or consumption, especially when followed by a signal.

Pipe input-end drains queued bytes then closes stdin. Terminal input-end checks
current canonical/EOF settings, flushes a pending unterminated line and requests
EOF while preserving output. It does not physically half-close a PTY or promise
exit. A program may change settings or ignore EOF. `input_end_requested` records
queue acceptance; `input_state` later distinguishes `end_requested`, `ended`,
`unsupported`, `peer_closed`, `stopped` and `failed` (plus startup/open states).
Unsupported raw modes and conflicting control settings remain visible through
read/poll metadata. Unexpected I/O failures also carry `io_error`.

Use `term` to request direct-child termination or `kill` to force it, then poll
for exit. These signals do not target a process tree. Terminal control characters
have different foreground-group semantics. Final master close during release or
shutdown may hang up descendant slave holders; it does not prove all descendants
are gone. Worker run cancellation alone does not shut down retained processes.

## Host integration and lifetime

```cpp
auto store = std::make_shared<tools::intrinsic::ProcessSessionStore>(executor);
registry.add(std::make_shared<tools::intrinsic::ProcessToolSet>(store));
// Before stopping the context:
co_await store->shutdown();
```

The optional toolset `AsyncEventBus*` selects the confirmation bus. Without a
handler, RequireConfirm calls are refused. `tools/example/deepseek_chat.cpp` and
`loop/example/deepseek_chat.cpp` use the same dynamically loaded declarations;
`--tools` and `--skill` display the updated contract without a provider request.

The store protects its table with a short mutex and owns a canonical strand per
session. No mutex spans an await. Capture observation and cursor updates are one
strand transaction. Public asynchronous calls may originate on other contexts;
returned observations own their data. Startup failure rolls back the child,
I/O tasks, session publication and capacity reservation.

Shutdown is an explicit, cancellation-shielded lifetime fence: visit every
session, stop input, terminate/reap as possible, bound trailing output drainage,
join tasks and then report the first error. Destructors are a last-resort fallback
requiring a stopped context and joined executor threads, not a substitute for
shutdown. Child exit, capture ending and task joining are separate observations.

Declarations resolve from `SIMPLEX_PROCESS_SCHEMA_DIR`, then
`<exe_dir>/schemas/process`, then the build-time source directory. Missing or
invalid declarations disable the affected tool; missing optional skill guidance
does not remove otherwise valid tools. CMake installs all declarations/skill.

## v0.5 interface migration

- Both launchers add `tty`, default false; old LaunchSpec JSON remains pipe mode.
- `read_process.stream` is removed; use the generic `output` block.
- `send_process.close_input` is replaced by `end_input`.
- `input_closed` is replaced by acceptance-oriented `input_end_requested` and
  explicit admission/runtime states.
- `stdout`, `stderr`, `new_stdout` and `new_stderr` tool blocks become `output`.
- Lower-level raw stream APIs remain available; terminal capture is optional and
  is never duplicated into stdout/stderr. Rebuild C++ hosts/plugins together.

Tests cover YAML/settled-query agreement, approval and scheduling, real pipe/PTY
execution, delta/full/poll cursors, UTF-8 projection, input admission, unsupported
EOF, lifecycle rollback and shared capture limits. Final integration coverage:

- `hub/test/e2e/process-tty.test.js`: real worker, approval, cancellation without
  destroying an existing terminal, ordered input/EOF, merged output, panel text
  parsing and final release.
- `core/test/test_lifecycle_container.py`: terminal IDs across worker restart,
  exclusive ownership, terminal child reaping and descendant-held pipe shutdown.
- `installed_terminal_runtime` in `test_tools`: execute both backends from the
  staged installation; repeated PTY allocation failures roll back without
  breaking pipe launches when `SIMPLEX_TEST_NO_PTY=1`.
- CI's staged-runtime matrix hides source declarations and exercises the installed
  tools on Ubuntu 22.04 and AlmaLinux 9. Hub E2E also mounts an empty `/dev/pts`
  in a disposable runtime container to exercise actual terminal unavailability.

Run the negative test only in a container whose devpts has intentionally been
masked. Setting the expectation on a normal host is a test failure, not a backend
configuration switch. The release package contains no test executable; CI copies
it into its temporary staged layout for this check.
