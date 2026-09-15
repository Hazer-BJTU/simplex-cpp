# process — intrinsic toolset

Five tools that let a model run child processes: start one — a program
directly, or a command line through the platform's shell — wait for it (or just
look at it), read what it printed, and tell it something: more input, the end of
its input, or a signal to stop. A process outlives the call that started it, so
each one gets a **session id** the model uses to come back to it in later
turns.

Built on the package's shared core (`tools_intrinsic`, see
[`../../README.md`](../../README.md)), which carries the argument reading, the
result shape and the confirmation routing; this directory is only the process
domain. The manager underneath is `process/`'s `ProcessHandle`.

- [The model's view](#the-models-view) — the five tools, their schemas, the
  skill that says how they fit together, the shape of a result, and each
  tool's
- [Worked examples](#worked-examples)
- [The host's view](#the-hosts-view) — wiring, shutdown, internals

---

## The model's view

Every tool's name, description and argument schema are **declared in YAML**,
one file per tool under [`schemas/`](schemas/) — `spawn_process.yaml`,
`send_process.yaml`, and so on — and loaded when the tool is built. What this
section shows is therefore not a copy of something written in C++: it is the
declaration, and the file is what a model is actually sent. The prose there is
the same prose below; edit the file and rerun the tests.

The directory carries one more document that is not a tool at all:
[`schemas/skill.yaml`](schemas/skill.yaml), the set's **skill** — how the five
are used TOGETHER, which is the one thing no per-tool description can say. The
set loads it when it is built, and it reaches a model as one section of the host's
system prompt (`ToolRegistry::inject_skills()`), appended after the host's own
instructions; `ToolSet::skill()` is where a host reads it back, and
`deepseek_chat --skill` prints it in full. See
[The set's skill](#the-sets-skill).

Those files also restate each tool's `type`/`security` pair for the reader.
That part is documentation, not configuration: the loader does not read it, the
tool's `write_attributes()` is what a scheduler and the security policy act on,
and `test_tools` fails if the two ever disagree. See
[Where the declarations live](#where-the-declarations-live).

### How the tools fit together

Two calls start a child, and which one to reach for is a question about what is
being run rather than about how long it takes. **`run_command`** takes a command
*line* and runs it through the platform's shell, so pipes, redirections, `&&`,
`$VARS` and globs work; **`spawn_process`** takes a program and a list of
arguments and passes them verbatim, with nothing in between to parse them. Both
then wait a short while — 3000 ms for a command line, 5000 ms for a program —
for the child to finish, and that wait is what lets one call serve two jobs: an
ordinary command finishes inside it, so its exit code and its whole output come
back in the **same call** that started it, and only a child that outlives the
window becomes a session to come back to.

```
run_command   ─┐
               ├─► finished in time? ──► yes: exit code + whole output, done
spawn_process ─┘                     │
                                     └─► no: session_id ──┬──► poll_process   wait for one of them, or look
                                                          │                   at where they all stand
                                                          ├──► read_process   incremental output
                                                          └──► send_process   feed its stdin, close it,
                                                                              or signal it to stop
```

**Looking and waiting are one call.** `poll_process` reports the state of every
session (or the named ones), and its `wait_timeout_milliseconds` says how long
to wait for one of them to finish: the call returns the moment ANY of them has,
or when the deadline runs out, and either way the answer is every session in the
selection. Waiting for one named process is that call with a one-element list;
`wait_timeout_milliseconds: 0` is the plain "what is going on right now" look.
There is no separate wait tool and no separate listing tool because they would
have been the same call with a different number in it.

**Output reads are incremental by default.** Each session remembers how much
of its output has already been handed over, so `read_process` and
`poll_process` return only what is *new* — a poll loop does not re-read the
same text every turn. Pass `full: true` for the whole capture; a full read
leaves the incremental position alone, so it never steals bytes from a poll
loop.

**Sessions have to be let go.** A finished process keeps its session (and its
output) until it is released, so the model can still read it. Release with
`release` on a read, or `release_exited` on a poll. At most **32**
sessions may be retained at once — an exited-but-unreleased session still counts
— and spawn refuses past that, which is a failure the model reads and can act on
by releasing finished sessions.

**Session ids are never reused.** `proc_3` names one process for the life of the
host, and after that session is released the number is spent: the next spawn
gets a fresh one. A model's older context still says "proc_3 is the build I
started three turns ago", and an id that came back around would make
`send_process(proc_3)` end a process that model never saw.

**A shell only where one was asked for.** `run_command` runs its line through
the platform's own interpreter, so `|`, `>`, `*` and `&&` mean what they mean in
a shell. `spawn_process` runs the executable directly, so the same characters
reach the program as literal arguments and no glob is expanded — which is what
makes it the right call for a filename that contains one. A model that wants a
pipeline through `spawn_process` has to ask for `sh -c '...'` and get the
quoting right; with `run_command` that is the call itself.

### The set's skill

Everything above — the ordinary path through the five, what a session costs,
what to wait for and how long, what a denied confirmation means — also ships as a
document the model itself is given: [`schemas/skill.yaml`](schemas/skill.yaml),
the set's **skill**. A tool declaration answers "what does this call do"; the
skill answers what none of them can between them, and it is prose rather than
behaviour: a model that ignores it can still call every tool, so the
declarations stay the contract and the skill stays advice.

The document holds a `name`, an optional `title` (the prompt section's
heading), a one-line `description`, `keywords` for a host that selects skills,
and the `text` — markdown, carried into the prompt verbatim. Its format and the
rules it is held to are `tools/intrinsic/skill_declaration.hpp`; the type is
`tools::ToolSetSkill` (`tools/include/tools/tool_skill.hpp`).

It reaches a model through the prompt template, not the tool list:
`ToolSet::inject_skill()` appends one Growing section, named
`skill.<name>` (here `skill.process`), after whatever the host has already said,
and `ToolRegistry::inject_skills()` is the same call over every registered set
in registration order. A host injects them with the rest of its fixed context —
after the persona and the tool listing, before anything it rewrites per turn,
which is the layout rule the template's stability tiers exist for. The demo does
exactly that (`tools/example/deepseek_chat.cpp`) and prints the result with
`/skill` or `--skill`.

A skill that cannot be read is the mildest failure in this package: the loader
reports the file and the set carries **no** skill, while every one of its tools
stays routable — guidance is not a capability. What that costs is quiet, though
— a model that was never told how the tools fit together still uses them, just
worse — so `test_tools` loads the real file and holds it against the set: it
must name every tool that actually registered, and it must arrive in a prompt
unchanged. A tool renamed in code and not in the skill fails the build's tests
rather than leaving a model instructions about a call that no longer exists.

### Which calls need confirmation

| | tools | why |
| --- | --- | --- |
| **Asks first** | `spawn_process`, `run_command`, `send_process` | they change state outside this process — running a program or a command line, and telling a running one what to do: more input, the end of its input, or a signal |
| **Runs unattended** | `poll_process`, `read_process` | looking at what is already running changes nothing, and so does waiting for it |

A confirmation that nobody answers is a refusal, so an unattended host runs the
observing calls and refuses the rest.

### Which calls may run beside their neighbours

The host schedules a turn's calls in one batch, and each call declares whether it
may overlap the others. What that declaration describes is the effect a call has
**outside this host**:

| call | | why |
| --- | --- | --- |
| `poll_process` | runs beside others | it asks what the children are doing and, with a timeout, waits for one of them; neither ends a child, and the cursors and table entries it touches are this layer's own bookkeeping |
| `read_process` | runs beside others | same, `full` and delta and `release` alike |
| `spawn_process` | runs alone | it starts a process on the machine, and two launches in one batch contend for the same files |
| `run_command` | runs alone | the same, one shell further out: the command line is the machine's business too, and two of them in one batch contend for the same files |
| `send_process` | runs alone | the bytes are the child's next input and a signal ends it, so the order two calls arrive in is what the child gets; one carrying `close_input` can drop another outright |

"Runs beside others" means what it says about safety, not about determinism:
the session store is what makes an overlapping poll or read safe — the table is
serialised on the store's strand and each child's handle and cursors on its own
(strand per session), so no two calls can tear a snapshot or hand out the same
bytes twice. What is *not* promised is that a batch's result is independent of
its interleaving: a model that asks for the same session's new output twice in
one batch gets it split between the two calls, in whichever order the schedule
picked, and a `release` beside a read of the same session may or may not be seen
by it. That is an ambiguous question asked twice in one turn, not a hazard to
anything outside the host.

Calls that run alone go **first**, in call order, and nothing else in the batch
is in flight while they do — which is why a poll beside a `spawn_process`
already sees the session that spawn created, and why a `send_process` beside a
waiting `poll_process` lets the wait find the child already gone instead of
timing out.

---

### The result shape

Every tool answers with one text part, and it is written for a reader — the
model, and a human looking at the transcript over its shoulder — rather than
serialised for a program:

```text
session_id: proc_1
state: exited
exit_code: 0
executable: seq
arguments: ["1","5"]
pid: 4242
running_milliseconds: 12

stdout (10 bytes):
1
2
3
4
5

stderr: (empty)
```

- **A field is one line**, `name: value`. A string arrives as itself — a path, a
  command, a label — and so does everything else that fits on a line; an array
  or a value that spans lines is compact JSON.
- **A field with nothing in it writes no line at all.** An exit code that does
  not exist yet, an empty argument list, an absent working directory: the
  absence says it, and `exit_code:` followed by nothing would be a line to
  interpret.
- **A block is verbatim text under a header**: `stdout (10 bytes):` and then the
  bytes the child printed. Nothing is escaped, quoted or folded — the text is
  the text. `(empty)` is a stream that printed nothing, `(truncated, first N
  bytes)` is a capture cut at the limit, and a `---` rule sets one record (a
  poll's session) off from the next.

The reasons for this over a JSON object are in
`tools/intrinsic/tool_result.hpp`: an object has to carry every string inside a
string, so the one part a caller asked for — what the child printed — arrives
escaped, and the facts around it arrive between braces.

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

**A quick command finishes inside the window**, and the answer is the facts
about the call with the child's own text under them. `stdout` and `stderr` are
the bytes it printed — not a string with `\n` escapes in it, which is what a
JSON object would have made of them:

```text
session_id: proc_1
state: exited
exit_code: 0
executable: grep
arguments: ["-rn","TODO","src/"]
description: find TODOs
pid: 48231
running_milliseconds: 8
finished: true
output_complete: true

stdout (34 bytes):
src/main.cpp:12: // TODO

stderr: (empty)

hint: the process finished; its output is above. Call read_process with release to forget the session when done with it
```

Values a caller would otherwise have to un-escape arrive as themselves
(`executable: grep`, a path, a label); a block is introduced by a header that
names it and counts its bytes, and a stream that printed nothing says
`(empty)`. See [The result shape](#the-result-shape) for the rules, and
`tools/intrinsic/tool_result.hpp` for why it is this and not JSON.

The session is **kept**, not reaped, even though it already finished: its
output stays readable, and the model releases it when done (`release: true` on
a later `read_process`, or `release_exited` on a poll).

**`finished` and `output_complete` are two different facts**, and the second is
not implied by the first. `finished` is about the child: it exited inside the
window, and its exit code is readable. `output_complete` is about its pipes: the
capture finished too, so the text above is everything the process printed. They
part company when a command starts something else that inherits its output and
then exits itself — a launcher, a background job — because the descendant keeps
those pipes open after the child is gone:

```text
session_id: proc_2
state: exited
exit_code: 0
executable: sh
arguments: ["-c","sleep 30 & exit 0"]
pid: 48237
running_milliseconds: 3
finished: true
output_complete: false

stdout: (empty)

stderr: (empty)

hint: the process finished, but its output capture is not complete yet: the text above is what has arrived so far. Something may still hold its output open; call poll_process to collect the rest
```

**A program that outlives the window** comes back as just the id and the
in-progress state — no `exit_code` line at all, and no output slice:

```text
session_id: proc_3
state: running
executable: make
arguments: ["-j4"]
description: build
pid: 48244
running_milliseconds: 5000
finished: false
output_complete: false

hint: the process is still running; call poll_process with a wait_timeout_milliseconds to wait for it to finish, or read_process to read what it has printed so far
```

Raise `expected_runtime_milliseconds` for a command that legitimately needs
longer but is still worth waiting for; pass `0` to skip the wait entirely and
get a `session_id` right away, for a program the model already knows will run
for a while (a server, a watch).

A launch that fails — no such executable, a working directory that is not a
directory, a malformed environment entry — comes back as a failure naming what
went wrong, not as a session.

### `run_command`

Run a command **line** through the platform's shell — the same launch as
`spawn_process`, for the case where the command is written the way a shell reads
it. Pipes, redirections, `&&`, `$VARS`, globs and quoting all work, because a
shell is what parses them; nothing needs quoting twice, and the model never
names the interpreter.

```jsonc
{
  "command": "ls -l /tmp | wc -l",         // required: one line, as a shell reads it
  "working_directory": "/home/me/project", // defaults to the host's own cwd; "" is refused
  "environment": ["LANG=C"],               // "KEY=VALUE", merged over the inherited env
  "inherit_environment": true,             // default true
  "expected_runtime_milliseconds": 3000    // default 3000; 0 returns a session id at once
}
```

Only `command` is required. What the tool builds from it is the host's own
interpreter as the executable and the line as the single argument after the
flag that says "this is the command" — so `bash -c 'ls -l /tmp | wc -l'` is what
actually runs, and the `arguments` line of the result says so:

```text
session_id: proc_1
state: exited
exit_code: 0
executable: bash
arguments: ["-c","ls -l /tmp | wc -l"]
description: ls -l /tmp | wc -l
pid: 48255
running_milliseconds: 9
finished: true
output_complete: true

stdout (3 bytes):
42

stderr: (empty)

hint: the process finished; its output is above. Call read_process with release to forget the session when done with it
```

There is **no `description` argument**: the command is the session's label, so
every later report about the session — a poll's record, a read, a launch failure
— says which command it is about. A model holding several sessions can tell
them apart without remembering which call made which.

The interpreter is chosen by the tool, not by the caller: bash where the host
has one, the POSIX `sh` otherwise. Which one it got is visible in the
`executable` line, and the choice is deliberately not the model's — a command
line written the ordinary way then behaves the same either way, and nothing has
to know the host's shell in advance. (The name, not a path, is what the launch
receives: the manager resolves executables through PATH, so `bash` is looked up
the same way the model's own `spawn_process` calls are.)

**A command that outlives its window** is the case the shortcut's hint is
written for. The command did not fail and was not killed — it is still running,
as a session like any other:

```text
session_id: proc_2
state: running
executable: bash
arguments: ["-c","sleep 600"]
description: sleep 600
pid: 48261
running_milliseconds: 3001
finished: false
output_complete: false

hint: the command had not finished after 3000 ms, so it is still running in the background as the session above; it was not killed and its work is not lost. Call poll_process with a wait_timeout_milliseconds to wait for it to finish, read_process to read what it has printed so far, and send_process to signal it
```

The window is named because that is what ran out, and the three calls it points
at are the three that check on a background session. A caller that asked for
`expected_runtime_milliseconds: 0` gets the same paragraph without the window —
nothing was waited for at all, which is a request rather than a timeout.

### `poll_process`

The state of every session — and the family's **wait**: it returns as soon as
ANY one of the sessions it was asked about has finished, or when its deadline
runs out, and either way it reports ALL of them. This is the call for checking
on work started earlier and the call for waiting on it, because those were never
two questions: what a caller wants in both cases is the state of a set of
sessions, and the only thing that ever differed was how long it was willing to
wait for one of them to change.

```jsonc
{
  "session_ids": ["proc_1", "proc_2"],   // omit or leave empty for every session
  "wait_timeout_milliseconds": 30000,    // default 30000; 0 does not wait at all
  "include_output": true,                // default true
  "release_exited": false                // default false
}
```

All four are optional; `{}` waits for the first session to finish.

**What finishes the wait.** A session counts as *finished* when its child has
exited AND its output capture is complete — the same pair of facts
`spawn_process` reports as `finished` and `output_complete`, and for the same
reason: a child that exits while something it started still holds its
stdout/stderr open is gone with its output still arriving, and ending the wait
there would hand back an exit code with half its output. A session already in
that state ends the wait at once, without waiting at all — so this is also the
cheap way to ask "is it done yet".

```text
timed_out: false
waited_milliseconds: 412
finished_count: 1
session_count: 2
retained_session_count: 2

---

session_id: proc_1
state: exited
exit_code: 0
output_complete: true
executable: grep
arguments: ["-rn","TODO","src/"]
description: find TODOs
pid: 48231
running_milliseconds: 412

new_stdout (25 bytes):
src/main.cpp:12: // TODO

new_stderr: (empty)

---

session_id: proc_2
state: running
executable: make
arguments: ["-j4"]
description: build
pid: 48244
running_milliseconds: 5000

new_stdout: (empty)

new_stderr: (empty)
```

The header says **why the call came back** before it says what it found.
`timed_out: false` means one of the sessions finished; `true` means the deadline
came first, which is a result and not an error — every session is exactly as it
was, still in the table, and waitable again. `waited_milliseconds` is how long
the call actually watched, which is also how "it was already finished when I
asked" reads. `finished_count` counts the sessions that are finished in this
answer (there can be more than one — the ones that already were), and
`session_count` is how many sessions the answer describes.

One session is one record, set off by a `---` rule; `new_stdout` holds what that
session printed since it was last read, and is `(empty)` when there is nothing
new — while a poll that did not ask for output has no such block at all.
`exit_code` and `output_complete` have **no line** while a process runs: an
absent exit code is not a zero one, and a running capture is arriving rather
than incomplete. The `released` record appears only when `release_exited`
actually freed something, and output is reported *before* the session is freed,
so nothing is lost — with one honest exception: a session whose child has exited
but whose capture is still open is freed like any other exited session, and what
has not arrived yet goes with it. Do not ask for a reap and the rest of an
incomplete capture in the same call. `retained_session_count` counts what the
table is holding,
exited-but-unreleased sessions included — the same number the 32-session cap
counts, so it is not the number of *running* processes.

**`wait_timeout_milliseconds: 0` does not wait at all**: the call takes one look
and returns, which is what a model uses when it wants the state right now rather
than a wait. A selection that resolves to nothing — an empty table, or ids that
name no session — answers at once either way, since there is no child whose
ending could end the wait.

**One call, not one per session.** With a real timeout this replaces the poll
loop: a model watching five builds makes one call that returns when the first of
them finishes and tells it about all five, rather than one call per process per
turn. And a session that has exited but whose capture is not complete yet keeps
the wait going, so `output_complete: false` in the answer always comes with
`timed_out: true` — the descendant-holding-the-pipes case, reported rather than
hidden, with a hint naming the session and the way to collect the rest.

### `read_process`

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

```text
session_id: proc_1
state: running
executable: grep
arguments: ["-rn","TODO","src/"]
pid: 48231
running_milliseconds: 412
stream: both
full: false

stdout (25 bytes):
src/main.cpp:12: // TODO

stdout_bytes_read: 25

stderr: (empty)

stderr_bytes_read: 0

released: true
```

A stream whose block header says `(truncated, first N bytes)` printed more than
the capture limit (4 MiB shared between the two streams), and the text under it
stops there. `*_bytes_read` is the total handed over so far, across all reads.
`released` reports what actually happened, not what was asked: a running
process is never released, so it comes back `false`.

Unlike a poll, this reads ONE named session, and `full: true` reads the whole
capture rather than the delta — which is what to reach for when the delta has
already been consumed by a poll and the text is wanted again.

### `send_process`

Tell a running process something — the three strengths there are, in one tool:
more input, the end of its input, and a signal to stop.

```jsonc
{
  "session_id": "proc_1",   // required
  "input": "some text\n",  // verbatim; include "\n" if it reads by lines
  "close_input": false,     // default false: end-of-input after sending
  "signal": ""              // "" (default) none, "term" shut down, "kill" end now
}
```

A call must do at least one of the three: send something, close the input, or
name a signal. A call that sends nothing, closes nothing and names no signal is
refused as one that does nothing at all.

```text
session_id: proc_1
state: running
bytes_queued: 10
input_closed: false

note: the text is queued for the process's standard input; the process may not have read it yet
```

`bytes_queued`, not "delivered": the write is handed to a background pump and
this returns before the process reads it. To confirm it was consumed, read the
output back. Many programs read until end-of-input — for those, send the text
and then `close_input: true`, or nothing happens.

Ending a process is the same call, one strength further: **`term` asks, `kill`
ends.** `term` is a request the
program may handle (it is the one to prefer for anything that holds state or
writes files), while `kill` cannot be caught and loses whatever the program was
holding. The session stays readable either way, so nothing already printed is
lost — and nothing is waited for, which is why the result says how to confirm:

```text
session_id: proc_1
state: running
executable: sleep
arguments: ["600"]
pid: 48231
running_milliseconds: 1200
signal: term
signalled: true

hint: signal sent; call poll_process with a wait_timeout_milliseconds to confirm the process has ended
```

The signal is sent, but the death is noticed a moment later, so the result may
still report `state: running` — that is not a failed signal. Call `poll_process`
to wait for the end. The two halves can be asked for together, which is the case two
tools could not spell — feed a program its own quit command and then make sure
it went:

```jsonc
{ "session_id": "proc_1", "input": "quit\n", "signal": "term" }
```

A child that has already exited answers both halves honestly: nothing was
delivered, and one `warning` line says what was lost — `the process has already
exited, so the input was discarded`, `... so no signal was sent`, or both in one
line when the call asked for both.

---

## Worked examples

**Run a command and get its output.** One call — it finishes inside the
default window:

```text
spawn_process { "executable": "ls", "arguments": ["-la", "/tmp"], "description": "list /tmp" }

   session_id: proc_1
   state: exited
   exit_code: 0
   executable: ls
   arguments: ["-la","/tmp"]
   description: list /tmp
   pid: 48231
   running_milliseconds: 4
   finished: true
   output_complete: true

   stdout (48 bytes):
   total 48
   …

   stderr: (empty)
```

The session is kept in case its output is wanted again; release it once done —
the delta is empty because the spawn already handed the whole capture over:

```text
read_process { "session_id": "proc_1", "release": true }

   session_id: proc_1
   …
   stdout: (empty)

   stderr: (empty)

   released: true
```

**Watch a long build.** Spawn with a short window (or `0`) so it becomes a
session at once, then wait for it — one call that returns when it finishes, or
after the timeout:

```text
spawn_process   { "executable": "make", "arguments": ["-j4"], "description": "build",
                  "expected_runtime_milliseconds": 0 }
   session_id: proc_2
   state: running
   …
   finished: false

poll_process    { "session_ids": ["proc_2"], "wait_timeout_milliseconds": 0 }
   timed_out: true                     // a look, not a wait: still building
   waited_milliseconds: 1
   finished_count: 0
   session_count: 1
   retained_session_count: 1

   ---

   session_id: proc_2
   state: running
   …
   new_stdout (19 bytes):
   [ 10%] Building…

poll_process    { "session_ids": ["proc_2"], "wait_timeout_milliseconds": 60000 }
   timed_out: false                    // it finished inside the wait
   waited_milliseconds: 18422
   finished_count: 1
   session_count: 1
   retained_session_count: 1

   ---

   session_id: proc_2
   state: exited
   exit_code: 0
   output_complete: true
   …
   new_stdout (14 bytes):
   [100%] Built

poll_process    { "session_ids": ["proc_2"], "wait_timeout_milliseconds": 0,
                  "release_exited": true }
   …
   released: ["proc_2"]
```

**Feed a program on stdin.** `cat` reads until end-of-input, so it has to
become a session before there is anything to write to — pass `0` so the spawn
does not wait for a program that is waiting right back:

```text
spawn_process         { "executable": "cat", "description": "echo back",
                        "expected_runtime_milliseconds": 0 }
   session_id: proc_3
   state: running
   …
   finished: false

send_process         { "session_id": "proc_3", "input": "hello\n", "close_input": true }
   session_id: proc_3
   state: running
   bytes_queued: 6
   input_closed: true

poll_process         { "session_ids": ["proc_3"] }
   timed_out: false
   waited_milliseconds: 4
   finished_count: 1
   session_count: 1
   retained_session_count: 1

   ---

   session_id: proc_3
   state: exited
   exit_code: 0
   output_complete: true

   new_stdout (6 bytes):
   hello

   new_stderr: (empty)
```

**A pipeline is one call with `run_command`:**

```text
run_command { "command": "ls /tmp | wc -l" }

   session_id: proc_1
   state: exited
   exit_code: 0
   executable: bash
   arguments: ["-c","ls /tmp | wc -l"]
   …
   stdout (3 bytes):
   42
```

Through `spawn_process` the same thing has to spell the shell out —
`{ "executable": "sh", "arguments": ["-c", "ls /tmp | wc -l"] }` — and get the
quoting right; without it, `|` and `wc` would reach `ls` as literal arguments.

**Stop something that is taking too long:**

```text
poll_process { "session_ids": ["proc_4"], "wait_timeout_milliseconds": 5000 }
   timed_out: true                       // not an error: still running
   waited_milliseconds: 5001
   finished_count: 0
   session_count: 1

   ---

   session_id: proc_4
   state: running

send_process { "session_id": "proc_4", "signal": "kill" }
   signal: kill
   signalled: true

   hint: signal sent; call poll_process with a wait_timeout_milliseconds to confirm the process has ended

read_process { "session_id": "proc_4", "full": true, "release": true }
   stdout (… bytes):
   …everything it printed before it died…

   released: true
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

`tools/example/deepseek_chat.cpp` is that wiring in a running host: a live
provider conversation whose tools are these five, every call going through
`ToolRegistry::execute`, with an `InvokeConfirmEvent` handler at the terminal
that answers the RequireConfirm calls — plus the offline halves of what the
model is given: `--tools`, which prints the catalogue this section describes,
and `--skill`, which prints the guidance
[above](#the-sets-skill) in full. Neither needs a provider.

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

### The headers

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
- **`process/tools.hpp`** — the five `ToolInterface` implementations. Each
  checks its arguments in `ensure_arguments()` and writes the defaults into the
  query there (so the security check and the human confirmation see settled
  arguments), and answers with a `ToolResult` — field lines and the child's
  output verbatim. `InvokeType`
  describes what a call changes OUTSIDE the host: the two observing tools are
  `ReadOnly` (the cursors and table entries they touch are internal, the wait
  ends no child, and the store's strands make them safe to overlap), and the
  three that launch (a program, a command line), feed or end a process are
  `SerialWrite`. What each tool *is* — its name, its
  description and its argument schema — is not here: it is declared in
  `schemas/<tool>.yaml`, and `ProcessToolBase` loads it.
- **`process/toolset.hpp`** — the `ProcessToolSet` a host registers. Its name,
  its five tools, the store they share and the skill it loads; the catalogue, the
  routing, the build/release lifecycle and `skill()` come from
  `IntrinsicToolSet`, and `prepare()` / `execute()` stay as `ToolSet` defines
  them, since those carry the invocation layer's checkpoint sequence and failure
  contracts.
- **`process/schemas.hpp`** — where the declarations live, answered once (see
  below). The header a deployment's configuration question belongs in.

### Where the declarations live

`schemas/<tool name>.yaml`, one file per tool, next to this package's sources,
and each tool names its own when it is built
(`ProcessToolBase(store, "spawn_process.yaml", bus)`). What a file holds is the
tool's name, the prose a model reads and the JSON Schema of its arguments, and
nothing about how the tool behaves — the format, and what is deliberately not
loaded from it, is `tools/intrinsic/tool_declaration.hpp`.

The same directory holds `schemas/skill.yaml`, which the SET loads rather than a
tool (`ProcessToolSet`'s constructor calls `load_skill(schema_directory() /
"skill.yaml")`). It is a document of the same package and is resolved the same
way — one directory, one lookup rule — but it is not a fifth tool: it has no
argument schema and no call, and the tool-declaration loader would refuse it.

The directory is resolved in exactly one place, `process/schemas.hpp`:

1. `SIMPLEX_PROCESS_SCHEMA_DIR`, when it is set and non-empty — a deployment
   that keeps the declarations somewhere of its own choosing points this at its
   copy, with no rebuild;
2. otherwise `<exe_dir>/schemas/process`, when that directory exists — where a
   release installs them, following the same "beside the executable" convention
   as the host's `<exe_dir>/plugins` lookup, so a staged tree needs no path
   configuration at all;
3. otherwise the path CMake baked in from the source tree, which is what the dev
   tree and the test suite use.

Only one of (2) and (3) is ever really there — a release carries the first, a
build tree the second — so the rule is simply "take the one that exists".

A file that cannot be read, or that does not satisfy the loader's shape rules,
is reported through the log — with the file and the in-document path — and the
tool it declares is **not registered**: a model is never offered a tool whose
description and schema nobody could find, and the rest of the set is
unaffected. That failure mode is also why the files are listed among the
target's sources in `CMakeLists.txt`: an IDE shows them with the package, and
since nothing is compiled from them, editing one takes effect on the next run
without a rebuild. `skill.yaml` is listed there too and is read the same way,
with the milder outcome [the skill
section](#the-sets-skill) describes: the guidance is lost, the tools are not.

The five are also declared to be one **capability group** ("process",
`declare_capability_group()` in `src/toolset.cpp`), because a tool that fails to
arrive on its own costs itself and no more is the right rule for one broken file
but not a safe *state* for a family: four of the five leaves a model able to
start a process it cannot end. So a partial registration is one error line —
the group, the count, every missing member — and `capability_groups()` answers
the same for a host that wants to act on it, while a package carrying none of
the files is reported as the family being absent rather than as five failures.

Which leaves a file free to claim something the implementation does not do, so
`test_tools` closes that gap: for each of the five tools it loads the file,
asserts the catalogue entry is that document verbatim, and asks the
implementation the same questions the document answers — every property
validated with the declared kind, the default contract held in **both**
directions (every declared default the value really settled, on every call the
declaration allows, and nothing settled that the file does not declare), every
value clause probed from **both** sides (each declared enum member accepted and
one outside refused, the declared minimum accepted and one below it refused, a
string of exactly `minLength` accepted and a shorter one refused, an element of
the declared type accepted and one of another refused), each `anyOf`
alternative a call the tool accepts, the required-only call refused whenever the
declaration states a cross-property rule, the declared `required` really
required, and the restated `type`/`security` pair the pair the tool declares. A
declaration that stops describing its tool fails the suite — including one that
quietly drops a `default:`.

One rule deliberately lives on the implementation side of that line:
`environment` entries must be `"KEY=VALUE"`, which no keyword in the vocabulary
expresses (`items` says only what an element's *type* is). The file states it in
prose, `tools.cpp` enforces it, and `test_tools` pins it as an **unstated
rule** — an element the schema alone would allow and the implementation refuses
— rather than passing over it.

### Deliberately absent

No separate `wait_process`, and no read-only listing beside `poll_process`,
because those two were one call with two different timeouts: both answered "the
state of this set of sessions", and the wait is the same call with patience.
Keeping both names would have made "wait for a process" and "check on a process"
look like different operations when the second is the first with the timeout at
0 — and would have left a model choosing between synonyms. What a one-session
wait used to spell as a required `session_id` is now the one-element list, which
is the same call the multi-session case needs.

No reap tool — reaping is a flag on the reading tools, because the moment a dead
child's last output has been read is exactly when its session becomes garbage,
and a separate call would be a step to forget.

**The two launchers are two tools, not one tool with a mode.** They are the same
act — the whole of it, `ProcessToolBase::launch_and_report()`, is written once
and both go through it — but what a caller has to write to get a child running
is not: an executable and an argument list, or one line something has to
interpret. Folding them together would mean an `executable` that is sometimes a
program and sometimes a shell, an `arguments` array whose meaning depends on
which, or a model that has to know the host's interpreter and write `sh -c` with
the quoting right. So `run_command` names the platform's shell and hands it the
line, and `spawn_process` keeps its promise that nothing parses what it was
given — and the type/security pair is identical for the two, because the
difference is in what the caller writes rather than in what the call does.

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
  inherited the pipes and is still holding them. `wait_for_any()` waits for the
  pair on every session it was given and reports both facts per session;
  `spawn_process` reports `finished` and `output_complete` separately for the
  same reason. Pinned by a repeated test
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
for any of a set with and without a deadline, the refusal to release a live child (and a
concurrent release of one session having exactly one winner), both shutdown
paths — the last of them on a context with three worker threads, where a
strand mistake has somewhere to show up.

`test_tools` — each tool's result, every malformed argument's `ArgumentParse`
failure, the `Invoke` failure for a session that is gone, the type/security each
tool declares for a given settled call (asserted after settling, so a
`write_attributes` that never ran cannot pass), the defaults materialized into
the settled query, the unconfirmed-call refusal, and a whole turn through a
`ToolRegistry` batch. It also owns the declaration cross-check: every tool's
catalogue entry is compared against its `schemas/*.yaml` file, and the
implementation is asked the same questions the file answers — declared kinds,
defaults, enum members, minimums, `required` and the restated type/security
pair — so a declaration and its tool cannot drift apart. The same idea one level
up is the skill check: `schemas/skill.yaml` must load, must name every tool the
set registered, and must arrive in a prompt template unchanged.

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
