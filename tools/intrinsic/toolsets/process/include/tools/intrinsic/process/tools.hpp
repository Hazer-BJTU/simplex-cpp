#pragma once

//
// process/tools.hpp — the five process-management tools a model can call
// ======================================================================
//
// One ToolInterface per operation, over one shared ProcessSessionStore:
//
//   spawn_process         run a program: wait out its expected runtime, and
//                         answer with the whole result if it finished inside
//                         that window, or a live session id if it did not.
//   run_command           the same launch, for a command LINE: the platform's
//                         shell is the executable and the line is what it is
//                         asked to run, so pipes, redirections and globs work
//                         without a model spelling out `sh -c` by hand.
//   poll_process          the state of every session (or named ones), each
//                         with its new output — and the family's WAIT: it
//                         returns as soon as any one of them has finished, or
//                         when its deadline runs out. The "what is going on"
//                         call, the one that waits, and the one that also
//                         serves as inspect.
//   read_process          one session's output: the delta by default, the
//                         whole capture on request.
//   send_process          tell a running child something: more input, the end
//                         of its input, or a signal to stop.
//
// WHY THERE ARE TWO LAUNCHERS, AND WHY THEY ARE NOT ONE TOOL. spawn_process and
// run_command are the SAME ACT: start a child, wait a bounded while for it, and
// answer with the whole result if it finished inside that window or with a live
// session id if it did not. That act is implemented once, in
// ProcessToolBase::launch_and_report(), and both tools go through it — the
// initial-wait window covers the ordinary "run this and tell me what it said"
// case in a single call, and only a child that outlives the window becomes a
// session to follow. That is the split ProcessHandle's
// await_initial_execution() was built for, so honouring it costs nothing and
// saves the model a round trip on every quick command.
//
// What the two do not share is what a caller has to WRITE to get a child
// running, and that is the one part a tool cannot decide for the caller: with
// spawn_process the executable and each argument are separate values passed
// verbatim, while a command line is ONE string that something has to interpret.
// Folding the second into the first would mean an `executable` that is sometimes
// a program and sometimes a shell, an `arguments` array whose meaning depends on
// which — or a model that has to know the host's shell and write `sh -c` with
// the quoting right every time. So the shell is a tool rather than a mode of one:
// run_command names the platform's own interpreter and hands it the line, and
// spawn_process keeps its promise that nothing parses what it was given.
//
// There is no separate wait_process either, and no read-only listing beside
// poll_process, because those two were one call wearing different timeouts.
// Both answered the same question — the state of a set of sessions — and the
// only thing that ever differed was how long the caller would wait for one of
// them to change. So poll_process waits, with a deadline a caller sets (0 =
// do not wait: tell me where things stand), and answers with EVERY session in
// the selection however the wait ended: the child that finished is what the
// wait was for, and its siblings are the context it is read in. Waiting for one
// named session is this call with a one-element list; a plain listing is this
// call with a timeout of 0. Keeping two names for that would only have made a
// model pick between synonyms — and, worse, made "wait for a process" and
// "check on a process" look like different operations when the second is the
// first with no patience.
//
// Feeding a child, ending its input and ending the child are one tool for the
// same reason read and wait are two: they are the same act at different
// strengths, answered the same way and refused for the same reason, so
// `send_process` carries all three rather than making a model pick the right
// verb for "please stop". Reaping is not a tool either —
// it is a flag on the two reading tools (`release` / `release_exited`),
// because the moment a caller has read a dead child's last output is exactly
// the moment its session becomes garbage, and a separate call would just be a
// step to forget.
//
// WHAT EACH TOOL DECLARES, and why the two columns are not the same question:
//
//   tool                  InvokeType    InvokeSecurity
//   spawn_process         SerialWrite   RequireConfirm
//   run_command           SerialWrite   RequireConfirm
//   poll_process          ReadOnly      Trusted
//   read_process          ReadOnly      Trusted
//   send_process          SerialWrite   RequireConfirm
//
// InvokeType is about SCHEDULING — may this run beside its neighbours in a
// batch — and what it describes is the effect a call has OUTSIDE this host (the
// definition of the three values is at the enum itself: dataclass/model_io.hpp,
// InvokeType). InvokeSecurity is about TRUST — may this run unattended. Running
// an arbitrary executable or command line, and telling a running one what to do
// (feeding it, closing its input, signalling it) are state changes outside this
// process, so they ask (through the async bus's InvokeConfirmEvent, per
// security_check.hpp: no answer means refused). Looking at what is already
// running is not, so it
// does not ask — a confirmation prompt for "read the output you just asked for"
// trains a user to click through prompts, which costs more than it buys.
//
// INTERNAL BOOKKEEPING IS NOT AN EXTERNAL EFFECT, and keeping the two apart is
// what makes this column mean something. A delta read advances that session's
// cursor; `release` removes a table entry; `include_output: false` and
// `release_exited: false` skip both. Those are changes to THIS LAYER'S OWN
// state, and the store is what makes them safe to overlap: the table is
// serialised on the store's strand and each child's handle and cursors on its
// own (process/session_store.hpp, threading), so two calls touching one session
// cannot tear a snapshot or double-hand the same bytes. A call that changes
// nothing outside the host is ReadOnly, however much internal state it moves.
//
// WHAT ReadOnly DOES NOT PROMISE, stated here because it is the honest limit of
// the rule: it is a statement about effects, not about a batch's determinism.
// Two consuming reads of one session in the same batch split that session's new
// output between them, and which one gets the bytes depends on the schedule —
// the same is true of a read beside a `release` of the same session. That is a
// model asking twice for the same thing in one turn, not a data race and not an
// ordering hazard for anything outside the host, and the store's contracts
// (no torn reads, no double delivery, no undefined behaviour) hold either way.
//
// THE WRITES, by contrast, all change something outside: running a program,
// putting bytes in a live child's input, ending it. They are SerialWrite rather
// than ParallWrite because their ORDER is observable out there — two launches
// contend for the same files, two stdin writes are what the child reads in that
// order (and a `close_input` beside one of them can drop the other outright,
// since a send onto a closed channel is discarded). Note what does NOT decide
// that: the handle's stdin channel is thread-safe, so overlapping writes cannot
// corrupt memory. That is the store's concurrency contract doing its job, and
// it says nothing about whether two orders mean the same thing.
//
// (A scheduler keyed by RESOURCE — `process:proc_3` — could serialise only the
// calls naming the same session and let the rest overlap. Nothing here needs to
// change for that: the types stay as they are, and only the grouping gets
// finer.)
//
// WHERE THE WORK HAPPENS. Every invoke() goes through the store's coroutine
// interface and never touches a ProcessHandle directly: the handles live on
// per-session strands, and invoke() runs on whatever executor the registry
// gave the batch. The store owns that hop (process/session_store.hpp,
// threading), so a tool here is argument checking, one store call, and a
// result.
//
// WHAT COMES FROM THE SHARED CORE. Argument reading and validation, the
// settling of defaults into the query (so the confirmation and the record
// carry the call that actually runs), the result shape, the confirmation's bus
// routing and the Invocable's storage all live in
// IntrinsicTool (tools/intrinsic/tool_base.hpp), which every intrinsic toolset
// derives from. The rules those encode — arguments checked in
// ensure_arguments() and never in invoke(); a wrong type refused rather than
// coerced; results as field lines with the child's output verbatim under them
// (tools/intrinsic/tool_result.hpp) — are stated there and not restated per
// tool.
//
// What ProcessToolBase adds below is only what is specific to this family: the
// store, the session id argument, the "no such session" failure, and the wire
// shape of a session.
//
// WHERE THE MODEL-FACING DECLARATION LIVES. Each tool's name, description and
// argument schema are in schemas/<tool name>.yaml, next to this package's
// sources, and are loaded when the tool is built (schemas.hpp says where that
// directory is and how a deployment moves it; tools/intrinsic/tool_declaration.hpp
// says what a file holds). Those files restate the type/security table above
// for the reader, and the loader deliberately ignores that: InvokeType and
// InvokeSecurity are behaviour, declared in write_attributes() and nowhere
// else. test_tools.cpp pins the file against the implementation — every
// declared property, every default, and the restated pair — so a declaration
// that stopped describing its tool fails the suite instead of quietly
// misinforming whoever reads it.
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
#include "tools/intrinsic/tool_result.hpp"

namespace tools::intrinsic {

/// The names the tools are registered under — one place, so the toolset's
/// routing table, the tests and a host's allow-list cannot drift apart.
namespace tool_names {
inline constexpr std::string_view kSpawn = "spawn_process";
inline constexpr std::string_view kRun = "run_command";
inline constexpr std::string_view kPoll = "poll_process";
inline constexpr std::string_view kRead = "read_process";
inline constexpr std::string_view kSend = "send_process";
} // namespace tool_names

/**
 * What every process tool adds to DeclaredTool: the store it works through,
 * the declaration file it is named by, and the two things only this family
 * needs — the session id argument, and the "that session is gone" failure.
 *
 * Everything domain-neutral (the Invocable's storage, the argument accessors,
 * the result shape, the confirmation's bus) comes from the base
 * (tools/intrinsic/tool_base.hpp) and is not restated here.
 */
class ProcessToolBase : public DeclaredTool {
public:
    using StorePtr = std::shared_ptr<ProcessSessionStore>;

    /**
     * @param store the session table this tool works through.
     * @param declaration_file the file this tool is DECLARED in, named
     *        relative to schema_directory() — one file per tool, in this
     *        package's schemas/ directory. Loading it here, in the base, is
     *        what leaves a tool class with nothing to say about its own name,
     *        description or argument schema (tools/intrinsic/tool_declaration.hpp).
     * @param bus the bus a RequireConfirm call asks its confirmation question
     *        on; nullptr means the process-wide one. See IntrinsicTool.
     */
    explicit ProcessToolBase(StorePtr store, std::string_view declaration_file,
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

    /// One session as every tool that reports one writes it: id, state, pid,
    /// exit code, timing, in that order — the fields a reader scans before the
    /// output under them. Shared so the five tools describe a session the same
    /// way, and written into the result rather than returned as a value
    /// because the result is built in order and never taken apart
    /// (tools/intrinsic/tool_result.hpp). Output blocks are added by the tools
    /// that read it.
    static void write_session(tools::intrinsic::ToolResult& result,
                              const SessionSnapshot& snapshot);

    /// The launch arguments BOTH launching tools take — everything the two
    /// schemas have in common, settled the one way:
    ///
    ///   environment       settled (so `[]` is what "none" looks like), and
    ///                     each entry checked for the execve KEY=VALUE shape
    ///                     the manager would otherwise refuse at its own
    ///                     Environment stage — a failure that would reach the
    ///                     model as a launch that went wrong when it is really
    ///                     the model's own argument to fix.
    ///   inherit_environment, expected_runtime_milliseconds
    ///                     settled at their defaults, @p default_window being
    ///                     the tool's own constant (5000 for a program, 3000 for
    ///                     a command line) so each schema can state its own
    ///                     number.
    ///   working_directory validated IN PLACE and left as it came: it is the one
    ///                     optional property with no default to write back,
    ///                     because absent means "inherit the host's working
    ///                     directory" and there is no placeholder path that
    ///                     means that. An EMPTY string is refused rather than
    ///                     read as absent — the two are different calls and the
    ///                     empty one cannot be run.
    static void settle_launch_arguments(model_io::InvokeQuery& query,
                                        std::uint64_t default_window);

    /// The same arguments, read into `spec` — the pure half, called from
    /// invoke() on a query settle_launch_arguments() already completed.
    static void apply_launch_arguments(const model_io::InvokeQuery& query,
                                       process::LaunchSpec& spec,
                                       std::uint64_t default_window);

    /// The body the two launching tools share: run `spec`, then answer with all
    /// of it — the session's facts, whether the child finished inside the
    /// spec's initial-wait window, whether its capture is complete, and the
    /// child's output when there is any to report.
    ///
    /// It lives here, in the family's base, because spawn_process and
    /// run_command differ in exactly two things and this is not one of them:
    /// how the LaunchSpec is built (a program with its arguments, or a command
    /// line with a shell in front of it), and what @p still_running_hint says
    /// when the window ran out. Everything else — the fields in their order,
    /// the full-capture read that leaves the delta cursor alone, the three
    /// honest reports of `finished` versus `output_complete`, the translated
    /// launch failure — is the same answer to the same question and is written
    /// once, here.
    ///
    /// @param spec what to launch. The caller reads the settled query so the
    ///        launch carries exactly what the confirmation was asked about
    ///        (tool_base.hpp).
    /// @param still_running_hint the `hint` line for the case the window ran
    ///        out — the one part of the answer that depends on what the caller
    ///        asked for, since "still running" reads differently to a model that
    ///        ran a shell command line than to one that started a program.
    boost::asio::awaitable<model_io::Content> launch_and_report(
        process::LaunchSpec spec, std::string still_running_hint);

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
    ///
    /// Stated twice on purpose, and pinned: this is what ensure_arguments()
    /// settles, and the `default` in schemas/spawn_process.yaml is what a model
    /// reads before calling. test_tools.cpp loads that file and fails if the
    /// two numbers ever disagree.
    static constexpr std::uint64_t kDefaultExpectedRuntimeMilliseconds = 5000;

    explicit SpawnProcessTool(StorePtr store,
                             eventbus::AsyncEventBus* bus = nullptr);

    void ensure_arguments(model_io::InvokeQuery& query) const override;
    void write_attributes(model_io::InvokeQuery& query) const override;
    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& query) override;
};

/// Run a command LINE through the platform's shell — the same launch as
/// spawn_process, for a caller that holds a line rather than a program and its
/// arguments. SerialWrite / RequireConfirm, the pair spawn_process declares and
/// for the same reasons: it runs arbitrary code, and two commands in one batch
/// contend for the same files.
///
/// No `description` argument, unlike spawn_process: the command IS the label,
/// and the spec's description is filled in with it, so every later report about
/// the session — a poll's record, a read, a launch failure — says which command
/// it is about without the model having to remember which call made which
/// session.
///
/// The interpreter is the PLATFORM's, resolved when the call runs (src/tools.cpp)
/// rather than named by the model: bash where the host has one, the POSIX `sh`
/// otherwise, and it is passed to the launch as the absolute path it was found
/// at. So a command line written the ordinary way works on a host whose only
/// shell is `sh`, a model never has to know which one it is talking to, and
/// nothing the call puts in `environment` can change which shell reads the
/// line — the result's `executable` line says which file it got.
class RunCommandTool final : public ProcessToolBase {
public:
    /// How long a command is waited for before its child is left running in the
    /// background, when the call does not say.
    ///
    /// Shorter than spawn_process's 5000 ms, because of what the two are usually
    /// asked for: run_command is the everyday "run this line and show me what it
    /// said" call — a listing, a grep, a pipe between two small programs — and a
    /// line still running after three seconds is far more often a server or a
    /// watch than a command whose output is worth two more seconds of waiting.
    /// A caller that knows better says so per call, and 0 skips the wait.
    ///
    /// Stated twice on purpose, and pinned: this is what ensure_arguments()
    /// settles, and the `default` in schemas/run_command.yaml is what a model
    /// reads before calling. test_tools.cpp loads that file and fails if the two
    /// numbers ever disagree.
    static constexpr std::uint64_t kDefaultExpectedRuntimeMilliseconds = 3000;

    explicit RunCommandTool(StorePtr store,
                            eventbus::AsyncEventBus* bus = nullptr);

    void ensure_arguments(model_io::InvokeQuery& query) const override;
    void write_attributes(model_io::InvokeQuery& query) const override;
    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& query) override;
};

/// The state of every session (or the named ones), each with its new output —
/// and the family's wait: with `wait_timeout_milliseconds` the call returns as
/// soon as ANY one of the sessions has finished, or when the deadline runs out,
/// and either way it reports ALL of them. ReadOnly / Trusted: it changes
/// nothing outside this host — a wait ends no child — and the cursors and table
/// entries it touches are the store's own state, which the store's strands make
/// safe to overlap.
///
/// A timeout is not a failure: the result says `timed_out`, names how long it
/// actually waited, and the sessions are all still there to be waited on again.
/// It is also the one call that can hold a parallel branch open for its whole
/// deadline, which is why the deadline defaults to a bounded value rather than
/// to "forever".
class PollProcessTool final : public ProcessToolBase {
public:
    /// The default deadline, in milliseconds. Long enough for an ordinary
    /// command, short enough that a stuck child comes back as a result the
    /// model can act on rather than a hang.
    ///
    /// Stated twice on purpose, and pinned, exactly like SpawnProcessTool's:
    /// this is what ensure_arguments() settles, and the `default` in
    /// schemas/poll_process.yaml is what a model reads before calling.
    /// test_tools.cpp loads that file and fails if the two ever disagree.
    static constexpr std::uint64_t kDefaultWaitTimeoutMilliseconds = 30000;

    explicit PollProcessTool(StorePtr store,
                             eventbus::AsyncEventBus* bus = nullptr);

    void ensure_arguments(model_io::InvokeQuery& query) const override;
    void write_attributes(model_io::InvokeQuery& query) const override;
    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& query) override;
};

/// One session's captured output: the delta since the last read by default,
/// the whole capture with `full`. ReadOnly / Trusted, whether it consumes the
/// cursor or releases the session — both are this layer's bookkeeping.
class ReadProcessTool final : public ProcessToolBase {
public:
    explicit ReadProcessTool(StorePtr store,
                             eventbus::AsyncEventBus* bus = nullptr);

    void ensure_arguments(model_io::InvokeQuery& query) const override;
    void write_attributes(model_io::InvokeQuery& query) const override;
    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& query) override;
};

/// Tell a running child something, at any of the three strengths there are:
/// here is more input, here is the end of your input, stop. One tool rather
/// than two because they are one call with one set of outcomes, and because a
/// model asking a child to shut down should not have to know whether that is a
/// "write" or a "kill" — the `signal` argument says which it wants.
///
/// SerialWrite (the bytes are the child's next input and a signal ends it, so
/// the order two calls arrive in is what the child gets — and one carrying
/// `close_input` can drop another outright) / RequireConfirm (it is input to,
/// or the end of, a live process).
///
/// The signal is CARRIED, not delivered by this layer: `term` and `kill` map
/// onto the store's two terminate paths, so a deployment whose children are not
/// local processes — a sandbox, a container, a remote host — can answer them
/// without ever calling kill(2) (src/tools.cpp).
class SendProcessTool final : public ProcessToolBase {
public:
    explicit SendProcessTool(StorePtr store,
                             eventbus::AsyncEventBus* bus = nullptr);

    void ensure_arguments(model_io::InvokeQuery& query) const override;
    void write_attributes(model_io::InvokeQuery& query) const override;
    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& query) override;
};

} // namespace tools::intrinsic
