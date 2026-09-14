#include "tools/intrinsic/process/tools.hpp"

#include <format>
#include <optional>
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

nlohmann::json ProcessToolBase::session_json(const SessionSnapshot& snapshot)
{
    const process::ExecutionStatus& execution = snapshot.result.execution;
    nlohmann::json entry{
        {"session_id", snapshot.id},
        {"executable", snapshot.result.spec.executable},
        {"arguments", snapshot.result.spec.arguments},
        {"description", snapshot.result.spec.description},
        {"pid", snapshot.result.spec.pid},
        // The contract's own enum wording ("running" / "exited" / "unknown"),
        // through its ADL serialiser rather than a second spelling here.
        {"state", execution.state},
        {"running_milliseconds", execution.cumulative_execution_milliseconds},
    };
    // Omitted while the child lives, rather than sent as null: an exit code
    // that is absent is not an exit code that is zero.
    if (execution.exit_code) {
        entry["exit_code"] = *execution.exit_code;
    }
    if (snapshot.result.spec.working_directory) {
        entry["working_directory"] = *snapshot.result.spec.working_directory;
    }
    return entry;
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

    // The settled values, written back so the confirmation and the record show
    // the call that will run (tool_base.hpp). Every one of these has a default
    // and so every one of them is materialized.
    (void)settle_string(query, "description");
    (void)settle_bool(query, "inherit_environment", true);
    (void)settle_uint(query, "expected_runtime_milliseconds",
                      kDefaultExpectedRuntimeMilliseconds);

    // working_directory is the one optional property with NO default to write
    // back: absent means "inherit the host's working directory", and there is
    // no placeholder path that means that. So it is validated in place — and
    // an EMPTY string is refused rather than treated as absent, because the
    // two are different calls and the empty one cannot be run: a child cannot
    // be started in "". Refusing at ArgumentParse is the model's chance to fix
    // a typo; quietly inheriting the host's cwd would run the call somewhere
    // the model did not ask for.
    if (const std::string directory = optional_string(query, "working_directory");
        find_argument(query, "working_directory") != nullptr && directory.empty()) {
        bad_argument(
            "property \"working_directory\" must not be empty: omit it to "
            "inherit the host's working directory, or name a directory to "
            "start the process in");
    }
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
    spec.inherit_environment = optional_bool(query, "inherit_environment", true);
    // working_directory stays DISENGAGED when the call did not name one: absent
    // means "inherit the parent's cwd", and an empty string is not a path, so
    // there is nothing to materialize (ensure_arguments refuses the empty one
    // rather than pretending it meant absent).
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
        optional_uint(query, "expected_runtime_milliseconds",
                      kDefaultExpectedRuntimeMilliseconds);

    try {
        const SpawnResult spawned = co_await _store->spawn(std::move(spec));
        const std::optional<SessionSnapshot> snapshot =
            co_await _store->snapshot(spawned.id);
        if (!snapshot) {
            // Only reachable if the session went away between the spawn and
            // this read — nothing in this module does that, and answering
            // with the id alone is still a usable result.
            co_return json_content(nlohmann::json{{"session_id", spawned.id}});
        }

        nlohmann::json payload = session_json(*snapshot);
        payload["finished"] = spawned.finished;
        // The second, separate fact (SpawnResult): the child ended inside the
        // window, but a descendant holding the inherited pipes open keeps the
        // capture from being complete. Reported rather than folded into
        // `finished`, because a caller told only "finished" would read the
        // output below as the whole of what the child printed.
        payload["output_complete"] = spawned.output_drained;

        if (!spawned.finished) {
            // Still running: the id is the useful part of the answer, and the
            // output would be a partial slice the caller did not ask for.
            payload["hint"] = std::format(
                "the process is still running; call {} to check on it, {} to "
                "wait for it, {} to read its output",
                tool_names::kPoll, tool_names::kWait, tool_names::kRead);
            co_return json_content(std::move(payload));
        }

        // Finished inside the window, which is the whole point of waiting: the
        // caller gets what it would otherwise have needed a second call for.
        // The FULL capture, and read with full=true so the delta cursor is left
        // alone — a later read still reports everything, and nothing is
        // silently consumed here.
        if (const std::optional<OutputRead> read = co_await _store->read_output(
                spawned.id, OutputStream::Both, true)) {
            payload["stdout_text"] = read->standard_output.text;
            payload["stderr_text"] = read->standard_error.text;
            payload["stdout_truncated"] = read->standard_output.truncated;
            payload["stderr_truncated"] = read->standard_error.truncated;
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
            payload["hint"] = std::format(
                "the process finished, but its output capture is not complete "
                "yet: the text above is what has arrived so far. Something "
                "may still hold its output open; call {} to collect the rest",
                tool_names::kWait);
            co_return json_content(std::move(payload));
        }
        payload["hint"] = std::format(
            "the process finished; its output is above. Call {} with release "
            "to forget the session when done with it", tool_names::kRead);
        co_return json_content(std::move(payload));
    } catch (const process::ProcessException& failure) {
        // The launch failed: translated at this boundary into the tool
        // module's own failure type, at the Invoke checkpoint. what() already
        // carries the whole launch context (stage, executable, description),
        // which is exactly what the model needs to see.
        throw InvokeException(InvokeException::Stage::Invoke, failure.what());
    }
}

// ---- poll_processes ---------------------------------------------------------

PollProcessesTool::PollProcessesTool(StorePtr store, eventbus::AsyncEventBus* bus)
    // Name, description and argument schema: schemas/poll_processes.yaml.
    : ProcessToolBase(std::move(store), "poll_processes.yaml", bus)
{}

void PollProcessesTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    (void)settle_string_list(query, "session_ids");
    (void)settle_bool(query, "include_output", true);
    (void)settle_bool(query, "release_exited", false);
}

void PollProcessesTool::write_attributes(model_io::InvokeQuery& query) const
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
    // What ReadOnly does NOT promise is that a batch's RESULT is independent of
    // its interleaving: two consuming polls of one session in the same batch
    // split that session's new output between them, and which one gets it
    // depends on the schedule. That is a determinism caveat on a call the model
    // made twice, not a data race, and it is documented where a reader will
    // meet it (tools.hpp's header, and the README).
    query.type = model_io::InvokeType::ReadOnly;
    query.security = model_io::InvokeSecurity::Trusted;
}

boost::asio::awaitable<model_io::Content> PollProcessesTool::invoke(
    const model_io::InvokeQuery& query)
{
    const std::vector<ProcessSessionStore::SessionId> ids =
        optional_string_list(query, "session_ids");
    const bool include_output = optional_bool(query, "include_output", true);
    const bool release_exited = optional_bool(query, "release_exited", false);

    const std::vector<SessionSnapshot> snapshots =
        co_await _store->snapshots(ids);

    nlohmann::json entries = nlohmann::json::array();
    std::vector<ProcessSessionStore::SessionId> released;
    for (const SessionSnapshot& snapshot : snapshots) {
        nlohmann::json entry = session_json(snapshot);
        if (include_output) {
            // The delta, so a poll loop reports what is NEW rather than
            // re-sending the whole capture every turn.
            if (const std::optional<OutputRead> read =
                    co_await _store->read_output(
                        snapshot.id, OutputStream::Both, false)) {
                entry["new_stdout"] = read->standard_output.text;
                entry["new_stderr"] = read->standard_error.text;
                if (read->standard_output.truncated) {
                    entry["stdout_truncated"] = true;
                }
                if (read->standard_error.truncated) {
                    entry["stderr_truncated"] = true;
                }
            }
        }
        entries.push_back(std::move(entry));

        // Reaped only AFTER its output is in the payload above: the other
        // order would hand the model a released session it never got to read.
        if (release_exited && snapshot.exited) {
            if (co_await _store->release(snapshot.id)) {
                released.push_back(snapshot.id);
            }
        }
    }

    nlohmann::json payload{
        {"sessions", std::move(entries)},
        // RETAINED, not alive: an exited session stays in the table (and in
        // this count) until it is released, which is what the cap counts too.
        // The name says so rather than leaving the reader to work out which
        // number "live" was meant to be.
        {"retained_session_count", co_await _store->size()},
    };
    if (!released.empty()) {
        payload["released"] = released;
    }
    co_return json_content(std::move(payload));
}

// ---- read_process_output ----------------------------------------------------

ReadProcessOutputTool::ReadProcessOutputTool(StorePtr store, eventbus::AsyncEventBus* bus)
    // Name, description and argument schema: schemas/read_process_output.yaml.
    : ProcessToolBase(std::move(store), "read_process_output.yaml", bus)
{}

void ReadProcessOutputTool::ensure_arguments(model_io::InvokeQuery& query) const
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

void ReadProcessOutputTool::write_attributes(model_io::InvokeQuery& query) const
{
    // ReadOnly, unconditionally, for the reason given on PollProcessesTool: a
    // read touches nothing outside this host. `full` and the delta both leave
    // the child exactly as they found it; the delta advances a cursor and
    // `release` removes a table entry, and both of those are this layer's own
    // state, serialised by the store's strands.
    //
    // The determinism caveat is the same one: a batch that asks for the same
    // session's delta twice splits the bytes between the two calls, in whatever
    // order the schedule picked. Safe, and not promised to be in call order.
    query.type = model_io::InvokeType::ReadOnly;
    query.security = model_io::InvokeSecurity::Trusted;
}

boost::asio::awaitable<model_io::Content> ReadProcessOutputTool::invoke(
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

    nlohmann::json payload{
        {"session_id", id},
        {"stream", stream_word(stream)},
        {"full", full},
    };
    if (snapshot) {
        payload.update(session_json(*snapshot));
        // update() would otherwise let the snapshot's own session_id key win;
        // they are the same value, so this is only about keeping the shape
        // predictable.
        payload["session_id"] = id;
    }
    if (stream == OutputStream::Stdout || stream == OutputStream::Both) {
        payload["stdout_text"] = read->standard_output.text;
        payload["stdout_truncated"] = read->standard_output.truncated;
        payload["stdout_bytes_read"] = read->stdout_cursor;
    }
    if (stream == OutputStream::Stderr || stream == OutputStream::Both) {
        payload["stderr_text"] = read->standard_error.text;
        payload["stderr_truncated"] = read->standard_error.truncated;
        payload["stderr_bytes_read"] = read->stderr_cursor;
    }

    if (release) {
        // release() refuses a running child, so this is honest either way:
        // "released" says what actually happened, not what was asked for.
        payload["released"] = co_await _store->release(id);
    }
    co_return json_content(std::move(payload));
}

// ---- write_process_input ----------------------------------------------------

WriteProcessInputTool::WriteProcessInputTool(StorePtr store, eventbus::AsyncEventBus* bus)
    // Name, description and argument schema: schemas/write_process_input.yaml.
    : ProcessToolBase(std::move(store), "write_process_input.yaml", bus)
{}

void WriteProcessInputTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    (void)require_session_id(query);
    const std::string input = settle_string(query, "input");
    const bool close_input = settle_bool(query, "close_input", false);
    if (input.empty() && !close_input) {
        // Neither writing nor closing: the call would do nothing at all, and
        // silently succeeding at nothing is worse than saying so. Checked on
        // the SETTLED values, so the refusal and the payload agree on what an
        // absent `input` came to.
        bad_argument("nothing to do: provide \"input\" to send, or "
                     "\"close_input\": true to end the process's input");
    }
}

void WriteProcessInputTool::write_attributes(model_io::InvokeQuery& query) const
{
    // The one write among the reading-looking tools, and a write by the rule
    // that decides this column: what it changes is OUTSIDE the host. Bytes
    // queued for a live child's standard input are the child's next input, not
    // this layer's bookkeeping.
    //
    // SerialWrite rather than ParallWrite, and the reason is the same rule read
    // one level further: ParallWrite means the order between writers is not
    // observable, while here it is. Two writes to one child land in the order
    // they arrive — that is what the child reads — and when one of them carries
    // `close_input` the other can be DROPPED outright, since a send onto a
    // closed channel is discarded. So the batch's call order is what the child
    // sees, which is only true if the batch runs these one at a time.
    //
    // Note what does NOT decide it: the handle's stdin channel is a
    // concurrent_channel, so overlapping writes cannot corrupt memory. That is
    // the store's concurrency contract doing its job; it says nothing about
    // whether the two orders mean the same thing.
    //
    // RequireConfirm: it is input to a live process, which can do anything with
    // it.
    query.type = model_io::InvokeType::SerialWrite;
    query.security = model_io::InvokeSecurity::RequireConfirm;
}

boost::asio::awaitable<model_io::Content> WriteProcessInputTool::invoke(
    const model_io::InvokeQuery& query)
{
    const std::string id = require_session_id(query);
    std::string input = optional_string(query, "input");
    const bool close_input = optional_bool(query, "close_input", false);
    const std::size_t byte_count = input.size();

    if (!co_await _store->write_input(id, std::move(input), close_input)) {
        no_such_session(id);
    }

    nlohmann::json payload{
        {"session_id", id},
        {"bytes_queued", byte_count},
        {"input_closed", close_input},
    };
    // Queued, not delivered: write_input is fire-and-forget by design (the
    // pump owns delivery), so the result must not claim the child has read
    // it. A model that needs to know reads the output back.
    payload["note"] =
        "the text is queued for the process's standard input; the process may "
        "not have read it yet";
    if (const std::optional<SessionSnapshot> snapshot =
            co_await _store->snapshot(id)) {
        payload["state"] = snapshot->result.execution.state;
        if (snapshot->exited) {
            // Worth saying plainly: a write to a dead child is dropped by the
            // pump, and nothing else in the result would reveal that.
            payload["warning"] =
                "the process has already exited, so the input was discarded";
        }
    }
    co_return json_content(std::move(payload));
}

// ---- wait_process ----------------------------------------------------------

WaitProcessTool::WaitProcessTool(StorePtr store, eventbus::AsyncEventBus* bus)
    // Name, description and argument schema: schemas/wait_process.yaml.
    : ProcessToolBase(std::move(store), "wait_process.yaml", bus)
{}

void WaitProcessTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    (void)require_session_id(query);
    (void)settle_uint(query, "timeout_milliseconds", kDefaultTimeoutMilliseconds);
    (void)settle_bool(query, "release", false);
}

void WaitProcessTool::write_attributes(model_io::InvokeQuery& query) const
{
    // ReadOnly, unconditionally: waiting watches a child the host already
    // started and changes nothing outside this host. `release` removes the
    // session it waited on, which is the table's own bookkeeping — the store
    // serialises that on its strand, so a neighbour addressing the same id is
    // safe rather than corrupted.
    //
    // What ReadOnly means for a batch here is worth stating, because a wait is
    // the one call that can hold a parallel branch open for its whole timeout:
    // it may occupy the executor for that long, alongside anything else that
    // overlaps. That is still the right trade — SerialWrite would make every
    // wait block every other call in the turn — and it is why the timeout
    // defaults to a bounded value rather than to "forever".
    query.type = model_io::InvokeType::ReadOnly;
    query.security = model_io::InvokeSecurity::Trusted;
}

boost::asio::awaitable<model_io::Content> WaitProcessTool::invoke(
    const model_io::InvokeQuery& query)
{
    const std::string id = require_session_id(query);
    const std::uint64_t timeout =
        optional_uint(query, "timeout_milliseconds",
                      kDefaultTimeoutMilliseconds);
    const bool release = optional_bool(query, "release", false);

    const std::optional<SessionSnapshot> snapshot =
        co_await _store->wait_for_exit(id, timeout);
    if (!snapshot) {
        no_such_session(id);
    }

    nlohmann::json payload = session_json(*snapshot);
    payload["exited"] = snapshot->exited;
    // THREE facts, not two, because "the child is gone" and "the output is all
    // here" are settled by different tasks and a caller told only the first
    // would treat a partial capture as the whole result (SessionSnapshot).
    // The pair that matters is the descendant-holding-the-pipes case: the
    // direct child exits at once while something it started keeps stdout open,
    // so the wait can end with `exited: true, output_complete: false`.
    payload["output_complete"] = snapshot->output_drained;
    // A timeout is a result, not a failure: it says the wait ended without
    // reaching the COMPLETE condition — the child gone AND its capture
    // finished — and the session is still there to be waited on again. Defined
    // as the negation of that condition rather than of `exited` alone, so the
    // three fields cannot disagree: timed_out is exactly the case where the
    // answer below is not the finished article.
    payload["timed_out"] = !(snapshot->exited && snapshot->output_drained);

    // The whole capture, not the delta: a caller waiting for a command to
    // finish wants its output, and cannot know whether an earlier poll
    // already consumed part of it. A full read leaves the delta cursor alone,
    // so this does not steal bytes from a concurrent poll loop either.
    if (const std::optional<OutputRead> read =
            co_await _store->read_output(id, OutputStream::Both, true)) {
        payload["stdout_text"] = read->standard_output.text;
        payload["stderr_text"] = read->standard_error.text;
        payload["stdout_truncated"] = read->standard_output.truncated;
        payload["stderr_truncated"] = read->standard_error.truncated;
    }
    if (snapshot->exited && !snapshot->output_drained) {
        // Said in prose as well as in the fields: the cause is not something
        // the reader can see in the output itself.
        payload["hint"] = std::format(
            "the process has exited, but its output is still incomplete: "
            "something it started may hold its stdout/stderr open. Call {} "
            "again to collect the rest",
            tool_names::kWait);
    }

    if (release) {
        payload["released"] = co_await _store->release(id);
    }
    co_return json_content(std::move(payload));
}

// ---- kill_process ----------------------------------------------------------

KillProcessTool::KillProcessTool(StorePtr store, eventbus::AsyncEventBus* bus)
    // Name, description and argument schema: schemas/kill_process.yaml.
    : ProcessToolBase(std::move(store), "kill_process.yaml", bus)
{}

void KillProcessTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    (void)require_session_id(query);
    (void)settle_bool(query, "graceful", false);
}

void KillProcessTool::write_attributes(model_io::InvokeQuery& query) const
{
    // SerialWrite / RequireConfirm: ending a process is a state change
    // outside this host, and an unattended kill of the wrong session is not
    // recoverable.
    query.type = model_io::InvokeType::SerialWrite;
    query.security = model_io::InvokeSecurity::RequireConfirm;
}

boost::asio::awaitable<model_io::Content> KillProcessTool::invoke(
    const model_io::InvokeQuery& query)
{
    const std::string id = require_session_id(query);
    const bool graceful = optional_bool(query, "graceful", false);

    // The store answers false for BOTH "no such session" and "the child was
    // already gone", so the snapshot below is what tells them apart: a
    // session that exists and has exited was simply already finished, which
    // is not a failure.
    const bool signalled = co_await _store->terminate(id, graceful);
    const std::optional<SessionSnapshot> snapshot =
        co_await _store->snapshot(id);
    if (!snapshot) {
        no_such_session(id);
    }

    nlohmann::json payload = session_json(*snapshot);
    payload["signalled"] = signalled;
    payload["graceful"] = graceful;
    if (!signalled) {
        payload["note"] = "the process had already finished; no signal was sent";
    } else {
        // The signal is sent, but the death is observed by the handle's own
        // watcher a moment later, so this result may still say "running".
        // Saying so beats a caller concluding the kill failed.
        payload["note"] = std::format(
            "signal sent; call {} to confirm the process has ended",
            tool_names::kWait);
    }
    co_return json_content(std::move(payload));
}

} // namespace tools::intrinsic
