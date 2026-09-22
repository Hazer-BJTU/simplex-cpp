#include "tools/intrinsic/process/tools.hpp"

#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "tools/intrinsic/process/schemas.hpp"
#include "tools/invoke_exception.hpp"

namespace tools::intrinsic {
namespace {

// The stream selector's wire words. Both is the default: a model reading a
// command's output almost always wants stderr next to stdout. Kept here rather
// than in the shared core because "which of a process's streams" is this
// family's vocabulary, not every intrinsic tool's.
std::optional<OutputStream> parse_stream(std::string_view word)
{
    if (word == "stdout") return OutputStream::Stdout;
    if (word == "stderr") return OutputStream::Stderr;
    if (word == "both") return OutputStream::Both;
    return std::nullopt;
}

std::string_view stream_word(OutputStream stream)
{
    switch (stream) {
        case OutputStream::Stdout: return "stdout";
        case OutputStream::Stderr: return "stderr";
        case OutputStream::Both:   return "both";
    }
    return "both"; // unreachable; keeps -Wreturn-type quiet
}

/// What a send_process call asks a child to do about STOPPING — the `signal`
/// argument, which is how a process is ended: ending it is one of the three
/// things this one tool can be asked for, not a tool of its own.
///
/// None is the empty word, and it is the default: a call that only writes says
/// so by leaving the signal out rather than by naming one it does not want, and
/// the schema's enum makes the two live signals the only other words there are.
/// The distinction this enum draws is the one the store's terminate() draws —
/// ask, or end — rather than a signal NUMBER: which signal a deployment really
/// delivers is that deployment's business (this layer never calls kill(2)
/// itself), so a model asks for the INTENT and the host answers with the
/// mechanism.
enum class Signal { None, Terminate, Kill };

std::optional<Signal> parse_signal(std::string_view word)
{
    if (word.empty()) return Signal::None;
    if (word == "term") return Signal::Terminate;
    if (word == "kill") return Signal::Kill;
    return std::nullopt;
}

std::string_view signal_word(Signal signal)
{
    switch (signal) {
        case Signal::None:      return "";
        case Signal::Terminate: return "term";
        case Signal::Kill:      return "kill";
    }
    return ""; // unreachable; keeps -Wreturn-type quiet
}

/// The shell a run_command call is run BY, and the flag that tells it the next
/// argument is the command line rather than a script file or an option.
///
/// The model names a command, not an interpreter (schemas/run_command.yaml), so
/// which shell that is has to be answered here, once, and the answer is the
/// platform's own: bash where the host has one, and the POSIX sh otherwise —
/// which POSIX guarantees is there, and under which every command line that
/// does not reach for bash extensions behaves the same.
///
/// The interpreter is named by its CONVENTIONAL ABSOLUTE PATH, and handed to
/// the launch as a path: the manager uses a path that exists exactly as written
/// (process/src/process_handle.cpp, resolution step 1), so nothing the model
/// puts in `environment` — PATH included — can change which interpreter reads
/// the line. The result's `executable` line names the file that was chosen, so
/// the caller can see what its command was parsed by.
///
/// One branch only, because this whole toolset is POSIX: the session store
/// signals pids and includes <sys/types.h>, so there is no build of this layer
/// whose platform shell is something else.
struct Shell {
    std::string executable;
    std::string_view command_flag;
};

[[nodiscard]] Shell platform_shell()
{
    for (const std::string_view candidate : {"/bin/bash", "/usr/bin/bash"}) {
        std::error_code ignored;
        if (std::filesystem::is_regular_file(candidate, ignored)) {
            return Shell{std::string(candidate), "-c"};
        }
    }
    return Shell{"/bin/sh", "-c"};
}

} // namespace

// ---- ProcessToolBase --------------------------------------------------------

ProcessToolBase::ProcessToolBase(StorePtr store, std::string_view declaration_file,
                                 eventbus::AsyncEventBus* bus)
    // The declaration is loaded HERE, in the family's base, so a tool class
    // names its file and has nothing to say about its own name, description or
    // argument schema. A file that cannot be loaded is logged and leaves the
    // tool unnamed, which is how the set skips it (tool_declaration.hpp).
    : DeclaredTool(schema_directory() / declaration_file, bus),
      _store(std::move(store))
{}

std::string ProcessToolBase::require_session_id(
    const model_io::InvokeQuery& query)
{
    return require_string(query, "session_id",
                          "the id a previous spawn_process returned");
}

void ProcessToolBase::no_such_session(const std::string& id)
{
    invoke_failed(std::format(
        "no live process session named \"{}\". Call {} to see which sessions "
        "exist", id, tool_names::kPoll));
}

void ProcessToolBase::write_session(ToolResult& result,
                                    const SessionSnapshot& snapshot)
{
    const process::ExecutionStatus& execution = snapshot.result.execution;
    // What the process IS, then what it is doing: a reader meets the id the
    // call was about before the facts about it. Every one of these is a field
    // the format writes as itself — a path, a command, a label — because that
    // is what a reader wants to see (tool_result.hpp).
    result.field("session_id", snapshot.id);
    // The contract's own enum wording ("running" / "exited" / "unknown"),
    // through its ADL serialiser rather than a second spelling here.
    result.field("state", execution.state);
    // Omitted while the child lives, rather than written as nothing: an exit
    // code that is absent is not an exit code that is zero.
    if (execution.exit_code) {
        result.field("exit_code", *execution.exit_code);
    }
    result.field("executable", snapshot.result.spec.executable);
    result.field("arguments", snapshot.result.spec.arguments);
    result.field("description", snapshot.result.spec.description);
    if (snapshot.result.spec.working_directory) {
        result.field("working_directory",
                     *snapshot.result.spec.working_directory);
    }
    result.field("pid", snapshot.result.spec.pid);
    result.field("running_milliseconds",
                 execution.cumulative_execution_milliseconds);
}

void ProcessToolBase::settle_launch_arguments(model_io::InvokeQuery& query,
                                              std::uint64_t default_window)
{
    // Settled so the confirmation and the record carry the list the launch
    // gets, `[]` when the model named none (the two spellings launch the same
    // child: the manager merges the entries in, and an empty list merges
    // nothing).
    const std::vector<std::string> environment =
        settle_string_list(query, "environment");

    // Each entry must be the execve KEY=VALUE shape. The manager checks this
    // too and throws at its Environment stage, but that failure would arrive
    // at the model as an Invoke failure - a launch that went wrong - when it
    // is really the model's own argument to fix.
    for (std::size_t index = 0; index < environment.size(); ++index) {
        const auto equals = environment[index].find('=');
        if (equals == std::string::npos || equals == 0) {
            bad_argument(std::format(
                "property \"environment\"[{}] must have the form "
                "\"KEY=VALUE\" with a non-empty key, got \"{}\"",
                index, environment[index]));
        }
    }

    (void)settle_bool(query, "inherit_environment", true);
    (void)settle_uint(query, "expected_runtime_milliseconds", default_window);

    // working_directory is validated here and left as it came: see the header
    // on this method — absent is a meaning of its own, and "" is not a path.
    if (const std::string directory = optional_string(query, "working_directory");
        find_argument(query, "working_directory") != nullptr && directory.empty()) {
        bad_argument(
            "property \"working_directory\" must not be empty: omit it to "
            "inherit the host's working directory, or name a directory to "
            "start the process in");
    }
}

void ProcessToolBase::apply_launch_arguments(const model_io::InvokeQuery& query,
                                             process::LaunchSpec& spec,
                                             std::uint64_t default_window)
{
    spec.inherit_environment = optional_bool(query, "inherit_environment", true);
    // working_directory stays DISENGAGED when the call did not name one: absent
    // means "inherit the parent's cwd", and an empty string is not a path, so
    // there is nothing to materialize (settle_launch_arguments refuses the
    // empty one rather than pretending it meant absent).
    if (const std::string directory = optional_string(query, "working_directory");
        !directory.empty()) {
        spec.working_directory = directory;
    }
    // environment is always engaged here, because the settled call always
    // carries the list — `[]` when the model named none. The two spellings
    // launch the same child: the manager merges the entries in, and an empty
    // list merges nothing. So the dataclass's "no explicit entries" case is
    // still reachable, just written the one way the settled query has.
    spec.environment = optional_string_list(query, "environment");
    // The window the store races the child against. detach_on_timeout is the
    // store's to force (a killed child would leave an id naming nothing), so
    // it is deliberately not set here.
    spec.initial_wait_timeout_milliseconds =
        optional_uint(query, "expected_runtime_milliseconds", default_window);
}

boost::asio::awaitable<model_io::Content> ProcessToolBase::launch_and_report(
    process::LaunchSpec spec, std::string still_running_hint)
{
    try {
        const SpawnResult spawned = co_await _store->spawn(std::move(spec));
        const std::optional<SessionSnapshot> snapshot =
            co_await _store->snapshot(spawned.id);
        if (!snapshot) {
            // Only reachable if the session went away between the spawn and
            // this read — nothing in this module does that, and answering
            // with the id alone is still a usable result.
            ToolResult lone;
            lone.field("session_id", spawned.id);
            co_return lone.render();
        }

        ToolResult result;
        write_session(result, *snapshot);
        result.field("finished", spawned.finished);
        // The second, separate fact (SpawnResult): the child ended inside the
        // window, but a descendant holding the inherited pipes open keeps the
        // capture from being complete. Reported rather than folded into
        // `finished`, because a caller told only "finished" would read the
        // output below as the whole of what the child printed.
        result.field("output_complete", spawned.output_drained);

        if (!spawned.finished) {
            // Still running: the id is the useful part of the answer, the
            // output would be a partial slice the caller did not ask for, and
            // what to do about it is the caller's own sentence (the hint its
            // tool passed in).
            result.field("hint", std::move(still_running_hint));
            co_return result.render();
        }

        // Finished inside the window, which is the whole point of waiting: the
        // caller gets what it would otherwise have needed a second call for.
        // The FULL capture, and read with full=true so the delta cursor is left
        // alone — a later read still reports everything, and nothing is
        // silently consumed here.
        if (const std::optional<OutputRead> read = co_await _store->read_output(
                spawned.id, OutputStream::Both, true)) {
            // Verbatim, both streams, even when a stream printed nothing: the
            // caller asked for the output, and "stderr: (empty)" is part of
            // the answer.
            result.block("stdout", read->standard_output.text,
                         read->standard_output.truncated);
            result.block("stderr", read->standard_error.text,
                         read->standard_error.truncated);
        }
        // The session is kept, not reaped: its output stays readable, and the
        // caller decides when to let it go. Saying so beats a caller assuming
        // either way.
        if (!spawned.output_drained) {
            // The one case where "finished" alone would mislead: the text
            // above is what has arrived SO FAR. The usual cause is a child
            // that started something with the same stdout/stderr and exited
            // itself, leaving the pipes open in the descendant's hands. The
            // session is still the way to the rest of it.
            result.field("hint", std::format(
                "the process finished, but its output capture is not complete "
                "yet: the text above is what has arrived so far. Something "
                "may still hold its output open; call {} to collect the rest",
                tool_names::kPoll));
            co_return result.render();
        }
        result.field("hint", std::format(
            "the process finished; its output is above. Call {} with release "
            "to forget the session when done with it", tool_names::kRead));
        co_return result.render();
    } catch (const process::ProcessException& failure) {
        // The launch failed: translated at this boundary into the tool
        // module's own failure type, at the Invoke checkpoint. what() already
        // carries the whole launch context (stage, executable, description),
        // which is exactly what the model needs to see.
        throw InvokeException(InvokeException::Stage::Invoke, failure.what());
    }
}

// ---- spawn_process ----------------------------------------------------------

SpawnProcessTool::SpawnProcessTool(StorePtr store, eventbus::AsyncEventBus* bus)
    // Name, description and argument schema: schemas/spawn_process.yaml.
    : ProcessToolBase(std::move(store), "spawn_process.yaml", bus)
{}

void SpawnProcessTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    (void)require_string(query, "executable", "the program to run");
    // Element-wise, so a single non-string names its own index rather than
    // failing as a type error from inside the whole-array conversion.
    (void)settle_string_list(query, "arguments");
    // The settled label, written back so the confirmation and the record show
    // the call that will run (tool_base.hpp). run_command has no such argument:
    // there the command IS the label, and its tool fills the spec in directly.
    (void)settle_string(query, "description");
    // Everything the two launching tools share: environment (validated),
    // inherit_environment, expected_runtime_milliseconds, working_directory.
    settle_launch_arguments(query, kDefaultExpectedRuntimeMilliseconds);
}

void SpawnProcessTool::write_attributes(model_io::InvokeQuery& query) const
{
    // SerialWrite: launching a process changes the machine's state, and two
    // launches in one batch may well contend for the same files. RequireConfirm:
    // this is the call that runs arbitrary code, so it asks (and, per
    // security_check.hpp, no answer means refused).
    query.type = model_io::InvokeType::SerialWrite;
    query.security = model_io::InvokeSecurity::RequireConfirm;
}

boost::asio::awaitable<model_io::Content> SpawnProcessTool::invoke(
    const model_io::InvokeQuery& query)
{
    process::LaunchSpec spec;
    // Read through the pure accessors, on a query ensure_arguments() has
    // already settled: every property below is present by now, with the value
    // the confirmation was asked about (tool_base.hpp).
    spec.executable = require_string(query, "executable", "the program to run");
    spec.arguments = optional_string_list(query, "arguments");
    spec.description = optional_string(query, "description");
    apply_launch_arguments(query, spec, kDefaultExpectedRuntimeMilliseconds);

    // The launch and the whole report are the shared body; what this tool adds
    // is the one sentence that fits a program nobody told it how long to run.
    co_return co_await launch_and_report(
        std::move(spec),
        std::format(
            "the process is still running; call {} with a "
            "wait_timeout_milliseconds to wait for it to finish, or {} to "
            "read what it has printed so far",
            tool_names::kPoll, tool_names::kRead));
}

// ---- run_command ------------------------------------------------------------

RunCommandTool::RunCommandTool(StorePtr store, eventbus::AsyncEventBus* bus)
    // Name, description and argument schema: schemas/run_command.yaml.
    : ProcessToolBase(std::move(store), "run_command.yaml", bus)
{}

void RunCommandTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    // The whole call: one line, and the launch arguments it shares with
    // spawn_process. No `description` — the command is the label (invoke()).
    (void)require_string(query, "command", "the command line to run");
    settle_launch_arguments(query, kDefaultExpectedRuntimeMilliseconds);
}

void RunCommandTool::write_attributes(model_io::InvokeQuery& query) const
{
    // The pair spawn_process declares, for the same two reasons: a command line
    // runs arbitrary code (RequireConfirm — and, per security_check.hpp, no
    // answer means refused), and it changes the machine outside this process,
    // where two commands in one batch contend for the same files (SerialWrite).
    //
    // The shell in front of it does not change either answer: what the
    // interpreter is asked to do is exactly what was confirmed.
    query.type = model_io::InvokeType::SerialWrite;
    query.security = model_io::InvokeSecurity::RequireConfirm;
}

boost::asio::awaitable<model_io::Content> RunCommandTool::invoke(
    const model_io::InvokeQuery& query)
{
    const std::string command =
        require_string(query, "command", "the command line to run");
    const Shell shell = platform_shell();

    process::LaunchSpec spec;
    // The interpreter IS the executable, and the command line is ONE argument
    // after the flag that says so. Nothing here splits, quotes or escapes the
    // line: the shell's own parser is the only thing that reads it, which is
    // what makes `a | b`, `a && b`, `$VAR`, globs and quoting work — and what
    // makes this tool different from spawn_process, which promises that nothing
    // parses what it was given.
    spec.executable = shell.executable;
    spec.arguments = {std::string(shell.command_flag), command};
    // The session's label, and the reason this call needs no `description`
    // argument: every later report about the session — a poll's record, a read,
    // a launch failure — then says which command it is about, so a model
    // holding several sessions can tell them apart from a poll alone.
    spec.description = command;
    apply_launch_arguments(query, spec, kDefaultExpectedRuntimeMilliseconds);

    // What the caller is told when the window runs out. It is the one part of
    // the answer that differs from spawn_process's, and it says what a model
    // that just ran a command line needs to hear: the command did not fail, it
    // was not killed, it is still going — and what to call to check on it. The
    // window is named because THAT is what expired: a caller that expected a
    // quick command learns its expectation was wrong, and one that asked for 0
    // is told plainly that nothing was waited for at all.
    const std::string tail = std::format(
        " Call {} with a wait_timeout_milliseconds to wait for it to finish, "
        "{} to read what it has printed so far, and {} to signal it",
        tool_names::kPoll, tool_names::kRead, tool_names::kSend);
    const std::uint64_t window = optional_uint(
        query, "expected_runtime_milliseconds",
        kDefaultExpectedRuntimeMilliseconds);
    std::string hint =
        window == ProcessSessionStore::kNoInitialWait
            ? std::format("the command was started without waiting, so it is "
                          "running in the background as the session above; "
                          "nothing has been read from it yet.{}", tail)
            : std::format("the command had not finished after {} ms, so it is "
                          "still running in the background as the session "
                          "above; it was not killed and its work is not "
                          "lost.{}", window, tail);

    co_return co_await launch_and_report(std::move(spec), std::move(hint));
}

// ---- poll_process -----------------------------------------------------------

PollProcessTool::PollProcessTool(StorePtr store, eventbus::AsyncEventBus* bus)
    // Name, description and argument schema: schemas/poll_process.yaml.
    : ProcessToolBase(std::move(store), "poll_process.yaml", bus)
{}

void PollProcessTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    (void)settle_string_list(query, "session_ids");
    (void)settle_uint(query, "wait_timeout_milliseconds",
                      kDefaultWaitTimeoutMilliseconds);
    (void)settle_bool(query, "include_output", true);
    (void)settle_bool(query, "release_exited", false);
}

void PollProcessTool::write_attributes(model_io::InvokeQuery& query) const
{
    // ReadOnly, unconditionally — what the type describes is the effect this
    // call has OUTSIDE the host, and a poll has none: it asks the table what
    // the children are doing and (with `include_output`) takes each session's
    // new bytes. Both of those touch this layer's own bookkeeping — the table
    // and the cursors — and that bookkeeping is the store's concurrency
    // contract, not the scheduler's business: the store serialises it on two
    // levels of strand, so an overlapping poll cannot tear a snapshot or
    // double-hand a byte. `release_exited` removes a table entry, which is
    // again internal.
    //
    // The WAIT is ReadOnly for the same reason and it is the interesting half:
    // watching a child is not driving it. This call ends no process, sends it
    // nothing, and leaves every session exactly as it found it — the store's
    // wait_for_any() only re-reads what the handles publish. A model that wants
    // a child gone says so through send_process, which asks.
    //
    // What ReadOnly does NOT promise is that a batch's RESULT is independent of
    // its interleaving: two consuming polls of one session in the same batch
    // split that session's new output between them, and which one gets it
    // depends on the schedule. That is a determinism caveat on a call the model
    // made twice, not a data race, and it is documented where a reader will
    // meet it (tools.hpp's header, and the README).
    //
    // Worth stating here as well: this is the one call that can hold a parallel
    // branch open for its whole deadline. That is still the right trade —
    // SerialWrite would make every wait block every other call in the turn —
    // and it is why the deadline defaults to a bounded value rather than to
    // "forever".
    query.type = model_io::InvokeType::ReadOnly;
    query.security = model_io::InvokeSecurity::Trusted;
}

boost::asio::awaitable<model_io::Content> PollProcessTool::invoke(
    const model_io::InvokeQuery& query)
{
    const std::vector<ProcessSessionStore::SessionId> ids =
        optional_string_list(query, "session_ids");
    const std::uint64_t wait_timeout = optional_uint(
        query, "wait_timeout_milliseconds", kDefaultWaitTimeoutMilliseconds);
    const bool include_output = optional_bool(query, "include_output", true);
    const bool release_exited = optional_bool(query, "release_exited", false);

    // The wait and the report are one call, and this is where the two meet: the
    // store returns as soon as ANY session in the selection has finished (its
    // child exited AND its output capture complete) or when the deadline runs
    // out — and it hands back EVERY selected session either way. Which child
    // ended is not decided here; it is readable per session on the snapshots
    // below, and there can be more than one.
    const WaitOutcome outcome = co_await _store->wait_for_any(ids, wait_timeout);
    const std::vector<SessionSnapshot>& snapshots = outcome.snapshots;

    // Everything this call needs from the store is taken FIRST, so that the
    // result can be written in the order a reader wants it — the verdict, the
    // count of what the table retains, then one record per session — while the
    // count still describes the table this call LEAVES BEHIND rather than the
    // one it found (a poll that reaps exited sessions must not report them as
    // retained).
    //
    // The order between the two passes is the contract: every session's output
    // is read BEFORE anything is released, because a released session's bytes
    // are gone for good.
    std::vector<std::optional<OutputRead>> reads;
    if (include_output) {
        // The delta, so a poll loop reports what is NEW rather than re-sending
        // the whole capture every turn.
        reads.reserve(snapshots.size());
        for (const SessionSnapshot& snapshot : snapshots) {
            reads.push_back(co_await _store->read_output(
                snapshot.id, OutputStream::Both, false));
        }
    }

    std::vector<ProcessSessionStore::SessionId> released;
    if (release_exited) {
        for (const SessionSnapshot& snapshot : snapshots) {
            if (snapshot.exited && co_await _store->release(snapshot.id)) {
                released.push_back(snapshot.id);
            }
        }
    }

    // Counted from the snapshots rather than inferred from the wait's verdict:
    // more than one session can be finished here (the ones that already were
    // when the call arrived), and `exited` alone is not finished — a child
    // whose capture something else still holds open is the case this is here
    // to name.
    std::size_t finished_count = 0;
    std::vector<ProcessSessionStore::SessionId> unfinished_output;
    for (const SessionSnapshot& snapshot : snapshots) {
        if (snapshot.exited && snapshot.output_drained) {
            ++finished_count;
        } else if (snapshot.exited) {
            unfinished_output.push_back(snapshot.id);
        }
    }

    ToolResult result;
    // Why the wait ended, before what it found: a reader that knows this was a
    // deadline reads the records below as a snapshot, and one that knows a
    // child has finished reads them as a result.
    result.field("timed_out", outcome.timed_out);
    // What the waiting cost, which is also how a reader tells "it was already
    // finished when I asked" from "it finished just now".
    result.field("waited_milliseconds", outcome.waited_milliseconds);
    result.field("finished_count", finished_count);
    result.field("session_count", snapshots.size());
    // RETAINED, not alive: an exited session stays in the table (and in this
    // count) until it is released, which is what the cap counts too. The name
    // says so rather than leaving the reader to work out which number "live"
    // was meant to be.
    result.field("retained_session_count", co_await _store->size());

    for (std::size_t index = 0; index < snapshots.size(); ++index) {
        // One record per session, set apart from the neighbours: a poll is
        // about several things at once, and a reader has to be able to tell
        // where each one starts (tool_result.hpp, separate()).
        result.separate();
        write_session(result, snapshots[index]);
        // Only once the child is gone: a running process's capture is not
        // incomplete, it is still arriving, and "output_complete: false" would
        // read as a verdict on a live process.
        if (snapshots[index].exited) {
            result.field("output_complete", snapshots[index].output_drained);
        }
        if (include_output && reads[index]) {
            // A stream that printed nothing is still a block — "(empty)" is
            // the answer to "what is new" — while a call that did not ask for
            // output has none at all.
            result.block("new_stdout", reads[index]->standard_output.text,
                         reads[index]->standard_output.truncated);
            result.block("new_stderr", reads[index]->standard_error.text,
                         reads[index]->standard_error.truncated);
        }
    }

    if (!released.empty()) {
        result.separate();
        result.field("released", released);
    }
    if (!unfinished_output.empty()) {
        // Said in prose as well as in the per-session `output_complete`,
        // because the cause is not something the reader can see in the output
        // itself: the child is gone, and the pipe is still open in somebody
        // else's hands.
        std::string names;
        for (const ProcessSessionStore::SessionId& id : unfinished_output) {
            if (!names.empty()) {
                names += ", ";
            }
            names += id;
        }
        result.field("hint", std::format(
            "{} exited, but the output capture is not complete yet: something "
            "it started may still hold its stdout/stderr open. Call {} again to "
            "collect the rest",
            names, tool_names::kPoll));
    }
    co_return result.render();
}

// ---- read_process -----------------------------------------------------------

ReadProcessTool::ReadProcessTool(StorePtr store, eventbus::AsyncEventBus* bus)
    // Name, description and argument schema: schemas/read_process.yaml.
    : ProcessToolBase(std::move(store), "read_process.yaml", bus)
{}

void ReadProcessTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    (void)require_session_id(query);
    if (const std::string stream = settle_string(query, "stream", "both");
        !parse_stream(stream)) {
        bad_argument(std::format(
            "property \"stream\" must be one of \"stdout\", \"stderr\", "
            "\"both\", got \"{}\"", stream));
    }
    (void)settle_bool(query, "full", false);
    (void)settle_bool(query, "release", false);
}

void ReadProcessTool::write_attributes(model_io::InvokeQuery& query) const
{
    // ReadOnly, unconditionally, for the reason given on PollProcessesTool: a
    // read touches nothing outside this host. `full` and the delta both leave
    // the child exactly as they found it; the delta advances a cursor and
    // `release` removes a table entry, and both of those are this layer's own
    // state, serialised by the store's mutex and session strands.
    //
    // The determinism caveat is the same one: a batch that asks for the same
    // session's delta twice splits the bytes between the two calls, in whatever
    // order the schedule picked. Safe, and not promised to be in call order.
    query.type = model_io::InvokeType::ReadOnly;
    query.security = model_io::InvokeSecurity::Trusted;
}

boost::asio::awaitable<model_io::Content> ReadProcessTool::invoke(
    const model_io::InvokeQuery& query)
{
    const std::string id = require_session_id(query);
    // Already validated in ensure_arguments, so the optional is engaged.
    const OutputStream stream =
        *parse_stream(optional_string(query, "stream", "both"));
    const bool full = optional_bool(query, "full", false);
    const bool release = optional_bool(query, "release", false);

    const std::optional<OutputRead> read =
        co_await _store->read_output(id, stream, full);
    if (!read) {
        no_such_session(id);
    }
    // Read BEFORE any release below, so the report describes the session as
    // it was when its output was taken.
    const std::optional<SessionSnapshot> snapshot =
        co_await _store->snapshot(id);

    ToolResult result;
    if (snapshot) {
        write_session(result, *snapshot);
    } else {
        // The session was there for the read and gone for the snapshot, which
        // is still worth naming: the output below answers for it.
        result.field("session_id", id);
    }
    // What the call asked for, echoed back: the result stands on its own in a
    // transcript, where the call it answers may be well above it.
    result.field("stream", stream_word(stream));
    result.field("full", full);
    if (stream == OutputStream::Stdout || stream == OutputStream::Both) {
        // A block for each stream READ, even when it printed nothing: the
        // caller asked for this stream, and one that is not in the answer
        // would read as one that was not asked about.
        result.block("stdout", read->standard_output.text,
                     read->standard_output.truncated);
        result.field("stdout_bytes_read", read->stdout_cursor);
    }
    if (stream == OutputStream::Stderr || stream == OutputStream::Both) {
        result.block("stderr", read->standard_error.text,
                     read->standard_error.truncated);
        result.field("stderr_bytes_read", read->stderr_cursor);
    }

    if (release) {
        // release() refuses a running child, so this is honest either way:
        // "released" says what actually happened, not what was asked for.
        result.separate();
        result.field("released", co_await _store->release(id));
    }
    co_return result.render();
}

// ---- send_process -----------------------------------------------------------

SendProcessTool::SendProcessTool(StorePtr store, eventbus::AsyncEventBus* bus)
    // Name, description and argument schema: schemas/send_process.yaml.
    : ProcessToolBase(std::move(store), "send_process.yaml", bus)
{}

void SendProcessTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    (void)require_session_id(query);
    const std::string input = settle_string(query, "input");
    const bool close_input = settle_bool(query, "close_input", false);
    const std::string signal = settle_string(query, "signal");
    if (!parse_signal(signal)) {
        // Checked on the SETTLED value, so the refusal and the payload agree on
        // what an absent `signal` came to — and so a model that sent "SIGKILL"
        // is told the words there are rather than having it read as "none".
        bad_argument(std::format(
            "property \"signal\" must be one of \"\" (no signal), \"term\" or "
            "\"kill\", got \"{}\"", signal));
    }
    if (input.empty() && !close_input && signal.empty()) {
        // Nothing to write, nothing to close, nothing to send: the call would
        // do nothing at all, and silently succeeding at nothing is worse than
        // saying so. Checked on the SETTLED values, so the refusal and the
        // payload agree on what an absent `input` came to.
        bad_argument("nothing to do: provide \"input\" to send, "
                     "\"close_input\": true to end the process's input, or a "
                     "\"signal\" (\"term\" or \"kill\") to send it");
    }
}

void SendProcessTool::write_attributes(model_io::InvokeQuery& query) const
{
    // The one write among the reading-looking tools, and a write by the rule
    // that decides this column: what it changes is OUTSIDE the host. Bytes
    // queued for a live child's standard input are the child's next input, not
    // this layer's bookkeeping — and a signal ends the child outright.
    //
    // SerialWrite rather than ParallWrite, and the reason is the same rule read
    // one level further: ParallWrite means the order between writers is not
    // observable, while here it is. Two calls to one child land in the order
    // they arrive — that is what the child reads and what it does — and when
    // one of them carries `close_input` the other can be DROPPED outright,
    // since a send onto a closed channel is discarded. So the batch's call
    // order is what the child sees, which is only true if the batch runs these
    // one at a time.
    //
    // Note what does NOT decide it: the handle's stdin channel is a
    // concurrent_channel, so overlapping sends cannot corrupt memory. That is
    // the store's concurrency contract doing its job; it says nothing about
    // whether the two orders mean the same thing.
    //
    // RequireConfirm: it is input to a live process, which can do anything with
    // it — and, for the same reason, the signal that ends one.
    query.type = model_io::InvokeType::SerialWrite;
    query.security = model_io::InvokeSecurity::RequireConfirm;
}

boost::asio::awaitable<model_io::Content> SendProcessTool::invoke(
    const model_io::InvokeQuery& query)
{
    const std::string id = require_session_id(query);
    std::string input = optional_string(query, "input");
    const bool close_input = optional_bool(query, "close_input", false);
    // Already validated in ensure_arguments, so the optional is engaged.
    const Signal signal = *parse_signal(optional_string(query, "signal"));
    const std::size_t byte_count = input.size();

    // What the call asked for, in the order it has to happen: the input first,
    // the signal after. A child told to stop would rarely read what arrived
    // with the same breath, while a child told to quit and THEN stopped has had
    // its chance to.
    const bool writes = byte_count != 0 || close_input;
    const bool queued =
        writes && co_await _store->write_input(id, std::move(input), close_input);
    const bool signalled =
        signal != Signal::None &&
        co_await _store->terminate(id, signal == Signal::Terminate);

    const std::optional<SessionSnapshot> snapshot =
        co_await _store->snapshot(id);
    if (!snapshot && !queued && !signalled) {
        // Nothing landed and there is no session to describe: the call named an
        // id this table does not know. A signal-only call that DID land is not
        // this case — it answered for a session that was there a moment ago —
        // so it reports what happened rather than failing.
        no_such_session(id);
    }

    ToolResult result;
    result.field("session_id", id);
    if (snapshot) {
        result.field("state", snapshot->result.execution.state);
    }
    // Only the halves the call asked for: a signal-only call has no queued
    // bytes to report, and `bytes_queued: 0` would read as "it wrote nothing
    // when it meant to".
    if (writes) {
        result.field("bytes_queued", byte_count);
        result.field("input_closed", close_input);
    }
    if (signal != Signal::None) {
        // What was asked for, then whether it happened: the word is echoed back
        // because the record's arguments are the settled call the confirmer saw,
        // and this is what a reader compares against `signalled`.
        result.field("signal", signal_word(signal));
        result.field("signalled", signalled);
    }
    // Queued, not delivered: write_input is fire-and-forget by design (the
    // pump owns delivery), so the result must not claim the child has read it.
    // A model that needs to know reads the output back.
    if (byte_count != 0) {
        result.field("note",
                     "the text is queued for the process's standard input; the "
                     "process may not have read it yet");
    }
    if (signalled) {
        // The signal is sent, but the death is observed by the handle's own
        // watcher a moment later, so this result may still say "running".
        // Saying so beats a caller concluding the signal failed.
        result.field("hint", std::format(
            "signal sent; call {} with a wait_timeout_milliseconds to confirm "
            "the process has ended",
            tool_names::kPoll));
    }
    if (snapshot && snapshot->exited) {
        // Worth saying plainly: a call to a child that is already gone had
        // nowhere to land, and nothing else in the result would reveal it. What
        // it says depends on what was lost — the bytes, the signal, or both.
        std::string dropped;
        if (byte_count != 0) dropped = "the input was discarded";
        if (signal != Signal::None) {
            if (!dropped.empty()) dropped += " and ";
            dropped += "no signal was sent";
        }
        if (!dropped.empty()) {
            result.field("warning", std::format(
                "the process has already exited, so {}", dropped));
        }
    }
    co_return result.render();
}

} // namespace tools::intrinsic
