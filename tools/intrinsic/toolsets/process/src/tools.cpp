#include "tools/intrinsic/process/tools.hpp"

#include <format>
#include <optional>
#include <utility>
#include <vector>

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

ProcessToolBase::ProcessToolBase(StorePtr store, eventbus::AsyncEventBus* bus)
    : IntrinsicTool(bus), _store(std::move(store))
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
    : ProcessToolBase(std::move(store), bus)
{
    _details.name = std::string(tool_names::kSpawn);
    _details.description =
        "Run a program. Waits a short while for it to finish, so an ordinary "
        "command returns its exit code and its whole output in this one call. "
        "A program still running when that wait runs out keeps running in the "
        "background instead, and the result carries a session id for it: use "
        "poll_processes or read_process_output to follow it, write_process_input "
        "to feed it, wait_process to wait for its exit and kill_process to end "
        "it. Set expected_runtime_milliseconds when a command needs longer than "
        "the default to finish in-call, or 0 to skip the wait and get an id "
        "straight away. There is no shell - the program is run directly, so "
        "pipes, redirections and globs reach it as literal arguments; run them "
        "through 'sh' with '-c' explicitly if that is what you want.";
    _details.argument_schema = object_schema(
        nlohmann::json{
            {"executable", string_property(
                "Program to run: a name resolved through PATH ('grep') or a "
                "path ('/usr/bin/grep')")},
            {"arguments", string_list_property(
                "Arguments after the program name. Each element is one "
                "argument, passed verbatim - do not quote or escape them")},
            {"description", string_property(
                "Short label for this process, echoed back in every report "
                "about it")},
            {"working_directory", string_property(
                "Directory to start the process in. Defaults to the host's "
                "own working directory")},
            {"environment", string_list_property(
                "Extra environment entries as \"KEY=VALUE\" strings, merged "
                "over the inherited environment by key")},
            {"inherit_environment", bool_property(
                "Whether the process inherits the host's environment", true)},
            {"expected_runtime_milliseconds", uint_property(
                "How long to wait for the program to finish before letting it "
                "continue in the background. Raise it for a command expected "
                "to take a while but still worth waiting for; 0 returns a "
                "session id immediately without waiting",
                kDefaultExpectedRuntimeMilliseconds)},
        },
        {"executable"});
}

void SpawnProcessTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    (void)require_string(query, "executable", "the program to run");
    // Element-wise, so a single non-string names its own index rather than
    // failing as a type error from inside the whole-array conversion.
    (void)optional_string_list(query, "arguments");
    const std::vector<std::string> environment =
        optional_string_list(query, "environment");

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

    // Validated with the same accessors invoke() reads through, so a malformed
    // value fails HERE - as an argument the model can fix - rather than being
    // silently replaced by a default once the call is already running.
    (void)optional_string(query, "description");
    (void)optional_string(query, "working_directory");
    (void)optional_bool(query, "inherit_environment", true);
    (void)optional_uint(query, "expected_runtime_milliseconds",
                        kDefaultExpectedRuntimeMilliseconds);
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
    spec.executable = require_string(query, "executable", "the program to run");
    spec.arguments = optional_string_list(query, "arguments");
    spec.description = optional_string(query, "description");
    spec.inherit_environment = optional_bool(query, "inherit_environment", true);
    // The two optionals stay DISENGAGED when absent, which is not the same as
    // empty: an engaged-but-empty environment means "explicitly no extra
    // entries", and an empty working directory would be a path rather than
    // "inherit the parent's cwd" (dataclass/process_spec.hpp says so on both).
    if (const std::string directory = optional_string(query, "working_directory");
        !directory.empty()) {
        spec.working_directory = directory;
    }
    if (find_argument(query, "environment") != nullptr) {
        spec.environment = optional_string_list(query, "environment");
    }
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
    : ProcessToolBase(std::move(store), bus)
{
    _details.name = std::string(tool_names::kPoll);
    _details.description =
        "List the state of the process sessions: which are still running, "
        "which have exited and with what code, and what each has printed "
        "since the last time its output was read. This is the call to make "
        "when checking on work started earlier. Pass session_ids to look at "
        "specific sessions, or leave it out for all of them.";
    _details.argument_schema = object_schema(nlohmann::json{
        {"session_ids", string_list_property(
            "Sessions to report on. Omit or leave empty for every session")},
        {"include_output", bool_property(
            "Whether to include each session's new output since the last "
            "read", true)},
        {"release_exited", bool_property(
            "Whether to forget sessions that have exited, after reporting "
            "them. Their ids stop being valid", false)},
    });
}

void PollProcessesTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    (void)optional_string_list(query, "session_ids");
    (void)optional_bool(query, "include_output", true);
    (void)optional_bool(query, "release_exited", false);
}

void PollProcessesTool::write_attributes(model_io::InvokeQuery& query) const
{
    // ReadOnly even though a delta read advances a cursor: the cursor is this
    // layer's record of what the model has been shown, not state any
    // neighbouring call observes, and it is serialised per session by that
    // session's strand (see the file header). Trusted: looking at processes
    // the host already started needs no permission.
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
        {"live_session_count", co_await _store->size()},
    };
    if (!released.empty()) {
        payload["released"] = released;
    }
    co_return json_content(std::move(payload));
}

// ---- read_process_output ----------------------------------------------------

ReadProcessOutputTool::ReadProcessOutputTool(StorePtr store, eventbus::AsyncEventBus* bus)
    : ProcessToolBase(std::move(store), bus)
{
    _details.name = std::string(tool_names::kRead);
    _details.description =
        "Read what one process has printed. By default this returns only what "
        "is new since the last read of that session, so it can be called "
        "repeatedly while a process runs without re-reading the same text; "
        "pass full to get everything captured so far instead.";
    _details.argument_schema = object_schema(
        nlohmann::json{
            {"session_id", string_property(
                "The session to read, as returned by spawn_process")},
            {"stream", enum_property("Which stream to read",
                                     {"stdout", "stderr", "both"}, "both")},
            {"full", bool_property(
                "Whether to return everything captured so far instead of only "
                "what is new. A full read does not consume the new output",
                false)},
            {"release", bool_property(
                "Whether to forget the session after reading it. Only applies "
                "once the process has exited", false)},
        },
        {"session_id"});
}

void ReadProcessOutputTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    (void)require_session_id(query);
    if (const std::string stream = optional_string(query, "stream", "both");
        !parse_stream(stream)) {
        bad_argument(std::format(
            "property \"stream\" must be one of \"stdout\", \"stderr\", "
            "\"both\", got \"{}\"", stream));
    }
    (void)optional_bool(query, "full", false);
    (void)optional_bool(query, "release", false);
}

void ReadProcessOutputTool::write_attributes(model_io::InvokeQuery& query) const
{
    // ReadOnly / Trusted for the reasons given on PollProcessesTool: the
    // cursor is bookkeeping, and reading output the host already captured
    // needs no confirmation.
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
    : ProcessToolBase(std::move(store), bus)
{
    _details.name = std::string(tool_names::kWrite);
    _details.description =
        "Send text to a running process's standard input. Include the "
        "trailing newline if the process reads by lines. Pass close_input to "
        "signal end-of-input afterwards, which is what a process reading "
        "until EOF waits for.";
    _details.argument_schema = object_schema(
        nlohmann::json{
            {"session_id", string_property(
                "The session to write to, as returned by spawn_process")},
            {"input", string_property(
                "Text to send, verbatim. Include \"\\n\" if the process reads "
                "lines")},
            {"close_input", bool_property(
                "Whether to close standard input after sending, so the "
                "process sees end-of-input", false)},
        },
        {"session_id"});
}

void WriteProcessInputTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    (void)require_session_id(query);
    const nlohmann::json* input = find_argument(query, "input");
    (void)optional_string(query, "input");
    const bool close_input = optional_bool(query, "close_input", false);
    if (input == nullptr && !close_input) {
        // Neither writing nor closing: the call would do nothing at all, and
        // silently succeeding at nothing is worse than saying so.
        bad_argument("nothing to do: provide \"input\" to send, or "
                     "\"close_input\": true to end the process's input");
    }
}

void WriteProcessInputTool::write_attributes(model_io::InvokeQuery& query) const
{
    // ParallWrite rather than SerialWrite: the handle's stdin channel is a
    // concurrent_channel — its documented thread-safe entry point — so this
    // needs no exclusive turn on the executor, only to be counted as a write.
    // RequireConfirm: it is input to a live process, which can do anything
    // with it.
    query.type = model_io::InvokeType::ParallWrite;
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
    : ProcessToolBase(std::move(store), bus)
{
    _details.name = std::string(tool_names::kWait);
    _details.description =
        "Wait for a process to finish and report how it ended, along with "
        "everything it printed. Returns as soon as the process exits, or when "
        "the timeout runs out — a timeout is not an error, the result simply "
        "reports the process as still running, and it keeps running.";
    _details.argument_schema = object_schema(
        nlohmann::json{
            {"session_id", string_property(
                "The session to wait for, as returned by spawn_process")},
            {"timeout_milliseconds", uint_property(
                "How long to wait before giving up and reporting the process "
                "as still running. 0 waits indefinitely, which risks waiting "
                "forever on a process that never exits",
                kDefaultTimeoutMilliseconds)},
            {"release", bool_property(
                "Whether to forget the session after it exits and its output "
                "has been reported", false)},
        },
        {"session_id"});
}

void WaitProcessTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    (void)require_session_id(query);
    (void)optional_uint(query, "timeout_milliseconds",
                        kDefaultTimeoutMilliseconds);
    (void)optional_bool(query, "release", false);
}

void WaitProcessTool::write_attributes(model_io::InvokeQuery& query) const
{
    // ReadOnly: waiting observes, it does not touch the child. Trusted: there
    // is nothing to authorise about watching a process the host already
    // started.
    //
    // Worth noting what ReadOnly means for a batch here: a wait may occupy a
    // parallel branch for its whole timeout. That is the correct trade —
    // SerialWrite would make the wait block every other call in the turn,
    // which is strictly worse — but it is why the timeout defaults to a
    // bounded value rather than to "forever".
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
    // A timeout is a result, not a failure: it says the wait ended without
    // the child ending, and the child is still there to be waited on again.
    payload["timed_out"] = !snapshot->exited;

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

    if (release) {
        payload["released"] = co_await _store->release(id);
    }
    co_return json_content(std::move(payload));
}

// ---- kill_process ----------------------------------------------------------

KillProcessTool::KillProcessTool(StorePtr store, eventbus::AsyncEventBus* bus)
    : ProcessToolBase(std::move(store), bus)
{
    _details.name = std::string(tool_names::kKill);
    _details.description =
        "End a running process. By default this kills it outright; pass "
        "graceful to ask it to shut down instead, which a process may handle "
        "or ignore. The session stays readable afterwards, so its output can "
        "still be collected.";
    _details.argument_schema = object_schema(
        nlohmann::json{
            {"session_id", string_property(
                "The session to end, as returned by spawn_process")},
            {"graceful", bool_property(
                "Whether to ask the process to shut down (a signal it may "
                "handle) instead of killing it outright", false)},
        },
        {"session_id"});
}

void KillProcessTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    (void)require_session_id(query);
    (void)optional_bool(query, "graceful", false);
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
