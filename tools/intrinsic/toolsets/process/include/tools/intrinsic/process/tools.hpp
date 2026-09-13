#pragma once

//
// process/tools.hpp — the six process-management tools a model can call
// =====================================================================
//
// One ToolInterface per operation, over one shared ProcessSessionStore:
//
//   spawn_process         run a program: wait out its expected runtime, and
//                         answer with the whole result if it finished inside
//                         that window, or a live session id if it did not.
//   poll_processes        the state of every session (or named ones), with
//                         each one's new output; the "what is going on"
//                         call, and the one that also serves as inspect.
//   read_process_output   one session's output: the delta by default, the
//                         whole capture on request.
//   write_process_input   feed a child's stdin, optionally closing it.
//   wait_process          block until one child exits, with a deadline.
//   kill_process          end a child, hard or graceful.
//
// WHY SIX AND NOT MORE. There is no separate run_command, because spawn_process
// IS one: its initial-wait window covers the ordinary "run this and tell me what
// it said" case in a single call, and only a program that outlives the window
// becomes a session to follow. That is the split ProcessHandle's
// await_initial_execution() was built for, so honouring it here costs nothing
// and saves the model a round trip on every quick command. Reaping is not a
// tool either —
// it is a flag on the two reading tools (`release` / `release_exited`),
// because the moment a caller has read a dead child's last output is exactly
// the moment its session becomes garbage, and a separate call would just be a
// step to forget.
//
// WHAT EACH TOOL DECLARES, and why the two columns are not the same question:
//
//   tool                  InvokeType    InvokeSecurity
//   spawn_process         SerialWrite   RequireConfirm
//   poll_processes        ReadOnly      Trusted
//   read_process_output   ReadOnly      Trusted
//   write_process_input   ParallWrite   RequireConfirm
//   wait_process          ReadOnly      Trusted
//   kill_process          SerialWrite   RequireConfirm
//
// InvokeType is about SCHEDULING — may this run beside its neighbours in a
// batch. InvokeSecurity is about TRUST — may this run unattended. Running an
// arbitrary executable, feeding it input, and killing it are all state
// changes outside this process, so they ask (through the async bus's
// InvokeConfirmEvent, per security_check.hpp: no answer means refused).
// Looking at what is already running is not, so it does not ask — a
// confirmation prompt for "read the output you just asked for" trains a user
// to click through prompts, which costs more than it buys.
//
// The reading tools are ReadOnly even though a delta read ADVANCES A CURSOR.
// That is not the observed system's state: it is this layer's bookkeeping
// about what the model has been shown, it is serialised per session by that
// session's strand, and two concurrent delta reads cannot tear or double-hand
// the same bytes. What ReadOnly promises a batch scheduler is that the call
// does not disturb what its neighbours observe — and a cursor no neighbour
// reads satisfies that. (The alternative, SerialWrite, would serialise every
// output read in a turn against every other tool call, which is a real cost
// for a bookkeeping detail.)
//
// WHERE THE WORK HAPPENS. Every invoke() goes through the store's coroutine
// interface and never touches a ProcessHandle directly: the handles live on
// per-session strands, and invoke() runs on whatever executor the registry
// gave the batch. The store owns that hop (process/session_store.hpp,
// threading), so a tool here is argument checking, one store call, and a JSON
// result.
//
// WHAT COMES FROM THE SHARED CORE. Argument reading and validation, the JSON
// result shape, the confirmation's bus routing and the Invocable's storage all
// live in IntrinsicTool (tools/intrinsic/tool_base.hpp), which every intrinsic
// toolset derives from. The rules those encode — arguments checked in
// ensure_arguments() and never in invoke(), because the security check and the
// human confirmation must see SETTLED arguments; a wrong type refused rather
// than coerced; results as a JSON object in a text part — are stated there and
// not restated per tool.
//
// What ProcessToolBase adds below is only what is specific to this family: the
// store, the session id argument, the "no such session" failure, and the wire
// shape of a session.
//

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <boost/asio/awaitable.hpp>
#include <nlohmann/json.hpp>

#include "dataclass/model_io.hpp"
#include "eventbus/async_event_bus.hpp"
#include "tools/intrinsic/process/session_store.hpp"
#include "tools/intrinsic/tool_base.hpp"

namespace tools::intrinsic {

/// The names the tools are registered under — one place, so the toolset's
/// routing table, the tests and a host's allow-list cannot drift apart.
namespace tool_names {
inline constexpr std::string_view kSpawn = "spawn_process";
inline constexpr std::string_view kPoll = "poll_processes";
inline constexpr std::string_view kRead = "read_process_output";
inline constexpr std::string_view kWrite = "write_process_input";
inline constexpr std::string_view kWait = "wait_process";
inline constexpr std::string_view kKill = "kill_process";
} // namespace tool_names

/**
 * What every process tool adds to IntrinsicTool: the store it works through,
 * and the two things only this family needs — the session id argument, and the
 * "that session is gone" failure.
 *
 * Everything domain-neutral (the Invocable's storage, the argument accessors,
 * the JSON result shape, the confirmation's bus) comes from the base
 * (tools/intrinsic/tool_base.hpp) and is not restated here.
 */
class ProcessToolBase : public IntrinsicTool {
public:
    using StorePtr = std::shared_ptr<ProcessSessionStore>;

    /**
     * @param store the session table this tool works through.
     * @param bus the bus a RequireConfirm call asks its confirmation question
     *        on; nullptr means the process-wide one. See IntrinsicTool.
     */
    explicit ProcessToolBase(StorePtr store,
                             eventbus::AsyncEventBus* bus = nullptr);

protected:
    /// The session id every tool but spawn takes. A thin wrapper over
    /// require_string() that supplies the one message worth writing once:
    /// where a model is supposed to have got the id from.
    [[nodiscard]] static std::string require_session_id(
        const model_io::InvokeQuery& query);

    /// The failure for a session the store does not know. Stage::Invoke, not
    /// ArgumentParse: "session_id must be a string" is a mistake in the call,
    /// while "proc_3 is gone" is a fact about the world that the arguments
    /// described correctly — a model fixes the first by re-reading its own
    /// call and the second by polling. The message says so.
    [[noreturn]] static void no_such_session(const std::string& id);

    /// The wire shape of one session, shared by every tool that reports one:
    /// id, state, pid, exit code, timing. Output is added by the tools that
    /// read it.
    [[nodiscard]] static nlohmann::json session_json(
        const SessionSnapshot& snapshot);

    StorePtr _store;
};

/// Launch a child process and return the session id that names it from now
/// on. SerialWrite / RequireConfirm — running an arbitrary executable is the
/// call this whole toolset exists to gate.
class SpawnProcessTool final : public ProcessToolBase {
public:
    /// How long a launch waits for the child before letting it continue in the
    /// background, when the call does not say.
    ///
    /// Five seconds is chosen for what it covers: the ordinary commands a model
    /// runs — a listing, a grep, a status, a small build step — finish well
    /// inside it and come back complete in one call, while a server or a watch
    /// crosses it and becomes a session. Too short and every command costs a
    /// second round trip; too long and a model waiting on a daemon looks stuck.
    /// A caller that knows better says so per call, and 0 skips the wait.
    static constexpr std::uint64_t kDefaultExpectedRuntimeMilliseconds = 5000;

    explicit SpawnProcessTool(StorePtr store,
                             eventbus::AsyncEventBus* bus = nullptr);

    void ensure_arguments(model_io::InvokeQuery& query) const override;
    void write_attributes(model_io::InvokeQuery& query) const override;
    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& query) override;
};

/// The state of every session (or the named ones), each with its new output.
/// ReadOnly / Trusted; `release_exited` reaps the dead ones after reading
/// them.
class PollProcessesTool final : public ProcessToolBase {
public:
    explicit PollProcessesTool(StorePtr store,
                             eventbus::AsyncEventBus* bus = nullptr);

    void ensure_arguments(model_io::InvokeQuery& query) const override;
    void write_attributes(model_io::InvokeQuery& query) const override;
    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& query) override;
};

/// One session's captured output: the delta since the last read by default,
/// the whole capture with `full`. ReadOnly / Trusted.
class ReadProcessOutputTool final : public ProcessToolBase {
public:
    explicit ReadProcessOutputTool(StorePtr store,
                             eventbus::AsyncEventBus* bus = nullptr);

    void ensure_arguments(model_io::InvokeQuery& query) const override;
    void write_attributes(model_io::InvokeQuery& query) const override;
    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& query) override;
};

/// Feed one child's stdin, optionally closing it afterwards. ParallWrite (the
/// handle's stdin channel is thread-safe, so this needs no exclusivity) /
/// RequireConfirm (it is input to a live process).
class WriteProcessInputTool final : public ProcessToolBase {
public:
    explicit WriteProcessInputTool(StorePtr store,
                             eventbus::AsyncEventBus* bus = nullptr);

    void ensure_arguments(model_io::InvokeQuery& query) const override;
    void write_attributes(model_io::InvokeQuery& query) const override;
    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& query) override;
};

/// Wait for one child to exit, with a deadline that defaults to nonzero: an
/// unbounded wait would hand a never-exiting child the agent loop. A timeout
/// is not a failure — the result says the child is still running.
/// ReadOnly / Trusted.
class WaitProcessTool final : public ProcessToolBase {
public:
    /// The default deadline, in milliseconds. Long enough for an ordinary
    /// command, short enough that a stuck child comes back as a result the
    /// model can act on rather than a hang.
    static constexpr std::uint64_t kDefaultTimeoutMilliseconds = 30000;

    explicit WaitProcessTool(StorePtr store,
                             eventbus::AsyncEventBus* bus = nullptr);

    void ensure_arguments(model_io::InvokeQuery& query) const override;
    void write_attributes(model_io::InvokeQuery& query) const override;
    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& query) override;
};

/// End a child: SIGKILL by default, SIGTERM with `graceful`. SerialWrite /
/// RequireConfirm.
class KillProcessTool final : public ProcessToolBase {
public:
    explicit KillProcessTool(StorePtr store,
                             eventbus::AsyncEventBus* bus = nullptr);

    void ensure_arguments(model_io::InvokeQuery& query) const override;
    void write_attributes(model_io::InvokeQuery& query) const override;
    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& query) override;
};

} // namespace tools::intrinsic
