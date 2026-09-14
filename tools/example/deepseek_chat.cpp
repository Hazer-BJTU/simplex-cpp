// tools/example/deepseek_chat.cpp — the tools-stack edition of the DeepSeek
// chat demo.
//
// llm/example/deepseek_chat.cpp drives the shipped provider machinery —
// LLMDispatcher loads the bundled DeepSeek plugin from <exe>/plugins/llm,
// create_model() mints a model, and the conversation runs through the LLMModel
// contract (converse + integrate) — but its tool side is a hand-rolled stub: a
// `calculate` Invocable built in the file and an if/else chain in
// execute_tool(). This example keeps the provider half and replaces the tool
// half with the tools module, so what runs here is the composition an agent
// host actually ships:
//
//   ToolRegistry          the host's ONE routing table. get_tools() flattens
//                         the catalogue the model is shown; execute() takes a
//                         whole turn's calls, settles every one of them
//                         (prepare: dispatch, ensure_arguments,
//                         write_attributes), runs the serial ones before the
//                         parallel ones, and answers with one record per call
//                         in call order.
//   ProcessToolSet        the intrinsic toolset the host links and constructs
//                         directly (no plugin, no dlopen): six ToolInterface
//                         implementations over one ProcessSessionStore.
//   ProcessSessionStore   the session table behind them — readable ids
//                         (proc_1, ...) that survive across turns, a strand
//                         per child, per-stream read cursors, and the
//                         terminate_all() shutdown path.
//   InvokeConfirmEvent    the module's security gate. spawn_process,
//                         write_process_input and kill_process declare
//                         RequireConfirm, so before any of them runs, the
//                         default policy publishes the settled call on the
//                         async event bus and waits for an answer. This file
//                         subscribes the one authority that can give it: the
//                         human at the terminal. Silence is not consent — with
//                         no handler subscribed, or with a handler that never
//                         answers, the call is refused and the model is told
//                         so, which is exactly what should happen to an
//                         unattended host.
//
// WHICH MAKES THIS EXECUTABLE THE DANGEROUS ONE, on purpose: the model can ask
// for real programs on this machine, and the confirmation prompt is the only
// thing between the two. That is the point — the toolset is worth testing end
// to end, and the gate is part of what is being tested — so the prompt shows
// the SETTLED call (defaults filled in, type and security written by
// write_attributes), not the raw one, and offers to remember a decision for
// the rest of the session: `a` approves this tool from now on, `A` approves
// everything. `--yes` skips the questions entirely, for a scripted run whose
// transcript is wanted rather than watched.
//
// WHAT THIS FILE DOES NOT DO: it never builds a ToolSet, never dispatches by
// name, never parses arguments, and never constructs an InvokeReturn. Every
// one of those belongs to the layers below — the set routes, the tools settle
// and run, the registry turns their results into records. The agent loop here
// is the small part: converse, integrate, hand the batch over, integrate the
// records.
//
// RUN MODES
//
//   (default)          the live conversation: needs an API key, streams
//                      reasoning to stderr, asks before every state-changing
//                      call.
//   --list-models      the provider's catalogue (plus the DeepSeek dialect's
//                      account-balance companion) over the same endpoint and
//                      credential, then exit. No conversation, no tools.
//   --tools            the tool catalogue ToolRegistry::get_tools() would hand
//                      the model — the six process tools as the intrinsic set
//                      loaded them from its YAML declarations, with the
//                      capability group's state. Needs NO API key and starts
//                      NO child, which is what makes it the offline smoke
//                      check the test suite registers (tools/example/
//                      CMakeLists.txt): a declaration that failed to load, a
//                      set that registered five of six tools, or a routing
//                      table that lost a name all exit non-zero here.
//   --yes              approve every confirmed call without prompting.
//   --max-steps N      how many model exchanges one user turn may take
//                      (default 12). Driving an interactive child — another
//                      agent, a REPL, a debugger — costs one exchange per
//                      "feed it, look at what it said" round, so that kind of
//                      work legitimately needs a larger budget than a chat
//                      answer does. Running out is not a failure: every call
//                      already ran and its result is in the conversation, so
//                      the next message continues from there.
//
// REPL COMMANDS (empty line quits): /tools /sessions /help

#include "eventbus/async_event_bus.hpp"
#include "eventbus/event_bus.hpp"
#include "llm/compat/chat_completions/events.hpp"
#include "llm/models.hpp"
#include "tools/intrinsic/process/session_store.hpp"
#include "tools/intrinsic/process/toolset.hpp"
// The family's own name vocabulary (tool_names::kSpawn, ...), so the smoke
// check below names the same six strings the set registers rather than a
// second copy of them.
#include "tools/intrinsic/process/tools.hpp"
#include "tools/intrinsic/toolset_base.hpp"
#include "tools/invoke_exception.hpp"
#include "tools/registry.hpp"
#include "tools/security_check.hpp"

#include <boost/asio.hpp>

#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace asio = boost::asio;

namespace {

// ---- configuration -----------------------------------------------------------

/// Default model. Override at runtime with the DEEPSEEK_MODEL environment
/// variable.
const char* model_name() {
    if (const char* env = std::getenv("DEEPSEEK_MODEL")) return env;
    return "deepseek-v4-flash";
}

/// How many model exchanges one user turn may take before the loop stops.
/// Overridable with --max-steps: driving an interactive child (another agent,
/// a REPL, a debugger) costs one exchange per round of "feed it, look at what
/// it said", and a task like that legitimately needs more than a chat answer
/// does.
constexpr std::size_t kDefaultMaxAgentSteps = 12;

struct Options {
    bool list_models_only = false;
    bool catalogue_only = false;
    bool assume_yes = false;
    bool help = false;
    std::size_t max_steps = kDefaultMaxAgentSteps;
};

void print_usage(const char* executable) {
    std::cout
        << "usage: " << executable
        << " [--tools] [--list-models] [--yes] [--max-steps N]\n"
        << "\n"
        << "  (no flags)      chat: one turn at a time, tools behind a\n"
        << "                  terminal confirmation prompt\n"
        << "  --tools         print the tool catalogue the registry would\n"
        << "                  give the model and exit (no API key needed,\n"
        << "                  starts no child process)\n"
        << "  --list-models   print the provider's live model list and\n"
        << "                  balance, then exit\n"
        << "  --yes           approve every confirmed tool call without\n"
        << "                  asking (scripted runs; the model still cannot\n"
        << "                  call a tool that does not exist)\n"
        << "  --max-steps N   model exchanges one user turn may take\n"
        << "                  (default " << kDefaultMaxAgentSteps
        << "; raise it to drive an\n"
        << "                  interactive child, which costs a round trip per\n"
        << "                  look)\n"
        << "\n"
        << "environment: DEEPSEEK_API_KEY (else prompted), DEEPSEEK_MODEL\n";
}

/// Parse the handful of flags this demo has. An unknown flag is a usage error
/// rather than something to ignore: a typo that silently changed whether the
/// model's shell commands need confirming would be the worst kind of quiet.
bool parse_options(int argc, char* argv[], Options& options) {
    for (int index = 1; index < argc; ++index) {
        std::string_view flag = argv[index];
        std::string_view value;
        // --max-steps N and --max-steps=N are both accepted; everything else
        // is a bare switch, so splitting the value off here keeps the chain
        // below to a plain comparison.
        if (const std::size_t equals = flag.find('=');
            equals != std::string_view::npos) {
            value = flag.substr(equals + 1);
            flag = flag.substr(0, equals);
        }

        if (flag == "--tools") {
            options.catalogue_only = true;
        } else if (flag == "--list-models") {
            options.list_models_only = true;
        } else if (flag == "--yes" || flag == "-y") {
            options.assume_yes = true;
        } else if (flag == "--help" || flag == "-h") {
            options.help = true;
        } else if (flag == "--max-steps") {
            if (value.empty()) {
                if (index + 1 >= argc) {
                    std::cerr << "--max-steps needs a number\n";
                    return false;
                }
                value = argv[++index];
            }
            std::size_t parsed = 0;
            const auto [end, error] = std::from_chars(
                value.data(), value.data() + value.size(), parsed);
            if (error != std::errc{} || end != value.data() + value.size() ||
                parsed == 0) {
                // One round is the floor that still lets a model call a tool
                // and see the result; zero would answer nothing at all.
                std::cerr << "--max-steps needs a positive number, got \""
                          << value << "\"\n";
                return false;
            }
            options.max_steps = parsed;
        } else {
            std::cerr << "unknown flag: " << flag << "\n";
            return false;
        }
    }
    return true;
}

// ---- renderers ---------------------------------------------------------------

const char* type_name(model_io::InvokeType type) {
    switch (type) {
        case model_io::InvokeType::ReadOnly:    return "read_only";
        case model_io::InvokeType::ParallWrite: return "parall_write";
        case model_io::InvokeType::SerialWrite: return "serial_write";
    }
    return "unknown";
}

const char* security_name(model_io::InvokeSecurity security) {
    switch (security) {
        case model_io::InvokeSecurity::DefaultDeny:    return "default_deny";
        case model_io::InvokeSecurity::RequireConfirm: return "require_confirm";
        case model_io::InvokeSecurity::Trusted:        return "trusted";
    }
    return "unknown";
}

/// A short, single-line summary of a declaration's description: enough to
/// recognise a tool in a listing without reprinting the paragraph a model
/// reads. Cut at a word boundary so nothing is half-printed.
std::string summary(std::string_view text, std::size_t limit = 140) {
    const std::size_t end = text.find('\n');
    const std::string_view line =
        end == std::string_view::npos ? text : text.substr(0, end);
    if (line.size() <= limit) return std::string(line);
    std::size_t cut = line.rfind(' ', limit);
    if (cut == std::string_view::npos) cut = limit;
    return std::string(line.substr(0, cut)) + " …";
}

// ---- the catalogue, as the registry presents it -------------------------------

/**
 * Print the flattened catalogue — ToolRegistry::get_tools(), the exact list
 * `state.tools` is set from below — with the process family's capability group.
 *
 * @return 0 when the six process tools are all routable and the family is
 *         whole, 2 otherwise. The non-zero exit is the offline smoke check:
 *         a schema file that failed to load, or a routing table that lost a
 *         name, fails here without touching a child process or the network.
 */
int report_catalogue(const tools::ToolRegistry& registry,
                     const tools::intrinsic::ProcessToolSet& process_set) {
    const std::vector<model_io::Invocable> catalogue = registry.get_tools();

    std::cout << "tool catalogue (ToolRegistry::get_tools())\n";
    std::cout << registry.size() << " toolset(s), " << catalogue.size()
              << " tool(s)\n";
    for (const model_io::Invocable& tool : catalogue) {
        std::cout << "  " << tool.name << " — " << summary(tool.description)
                  << "\n";
    }

    bool whole = true;
    for (const tools::intrinsic::IntrinsicToolSet::CapabilityGroup& group :
         process_set.capability_groups()) {
        std::cout << "capability group \"" << group.name << "\": ";
        if (group.missing.empty()) {
            std::cout << "whole (" << group.registered.size() << "/"
                      << group.registered.size() << ")\n";
        } else {
            std::cout << "DEGRADED — missing";
            for (const std::string& name : group.missing) {
                std::cout << " " << name;
            }
            std::cout << "\n";
            whole = false;
        }
    }

    // The group and the routing table are two different answers, so the smoke
    // check asks both: a name can be announced by the set and still be
    // unreachable if the registry refused it.
    for (const std::string_view name : {
             tools::intrinsic::tool_names::kSpawn,
             tools::intrinsic::tool_names::kPoll,
             tools::intrinsic::tool_names::kRead,
             tools::intrinsic::tool_names::kWait,
             tools::intrinsic::tool_names::kWrite,
             tools::intrinsic::tool_names::kKill,
         }) {
        if (!registry.contains(name)) {
            std::cerr << "the registry does not route \"" << name << "\"\n";
            whole = false;
        }
    }

    if (!whole) {
        std::cerr << "the process tool family is not whole; this demo needs "
                     "all six tools\n";
        return 2;
    }
    std::cout << "all six process tools are registered and routable\n";
    return 0;
}

// ---- the human in the loop ---------------------------------------------------

/**
 * The terminal confirmer: this host's answer to InvokeConfirmEvent.
 *
 * The process tools that change state outside this process declare
 * RequireConfirm, so default_security_check() publishes the SETTLED call on the
 * async event bus and refuses it unless a handler approves. This is that
 * handler, and it answers on behalf of the person running the demo: it prints
 * the call — name, id, arguments, and the type/security pair the tool declared
 * — and reads one keystroke.
 *
 * ONE QUESTION AT A TIME. The handler takes a mutex for the whole
 * prompt-and-read, because a batch may in principle confirm several calls at
 * once and two interleaved prompts would show the human a question and read
 * the answer for a different one. (No batch of THIS toolset can: the three
 * confirming tools are SerialWrite and the registry runs those one at a time.
 * The lock is here because that is a property of the set, and this file should
 * not depend on it for its own correctness.)
 *
 * THE BLOCKING READ IS DELIBERATE, and worth knowing the shape of: the
 * confirmation runs inside ToolSet::execute()'s security check, on the io
 * thread, BEFORE the call is scheduled — so the thread parked in std::getline
 * has no tool branch in flight behind it, and the registry's serial phase
 * means nothing else in this batch is waiting on that thread either. What it
 * does delay is the child processes' own bookkeeping timers (a session's reap
 * or an initial-wait deadline), which is harmless: they are deadline-driven,
 * not tick-driven, and the human is the reason the turn is slow.
 *
 * A DENIAL IS AN ANSWER, NOT AN ERROR: it comes back to the model as the
 * result of the call ("security check denied: ..."), so the model can adapt in
 * the next exchange instead of the turn dying. End-of-input denies and stops
 * asking for good — a piped-in script must not sit at a prompt that can never
 * be answered.
 */
class TerminalConfirmer {
public:
    explicit TerminalConfirmer(bool assume_yes) : _assume_yes(assume_yes) {}

    /// Subscribe on `bus` — the process-wide one by default, which is where
    /// the tools publish. The returned subscription must outlive the set.
    eventbus::AsyncEventBus::ScopedSubscription attach(
        eventbus::AsyncEventBus& bus) {
        return bus.subscribe<tools::InvokeConfirmEvent>(
            [this](tools::InvokeConfirmEvent event)
                -> asio::awaitable<tools::InvokeConfirmEvent> {
                co_return decide(std::move(event));
            });
    }

private:
    tools::InvokeConfirmEvent decide(tools::InvokeConfirmEvent event) {
        std::lock_guard<std::mutex> lock(_mutex);
        const model_io::InvokeQuery& query = event.query;

        // `d`, or end-of-input, already answered for everything that follows:
        // asking again would be a prompt nobody can answer.
        if (_deny_all) {
            event.decision = tools::ConfirmDecision::Denied;
            event.reason = "the session was told to deny the remaining "
                           "confirmations";
            return event;
        }
        if (_assume_yes) {
            event.decision = tools::ConfirmDecision::Approved;
            event.reason = "--yes: approved without asking";
            return event;
        }
        if (_always_allowed.count(query.name) != 0) {
            std::cout << "  [confirm] " << query.name
                      << " approved automatically (session rule)\n";
            event.decision = tools::ConfirmDecision::Approved;
            event.reason = "approved for this tool for the rest of the session";
            return event;
        }

        std::cout << "\n  [confirm] the model asked for a call that changes "
                     "state outside this host:\n"
                  << "            " << query.name << "  (call " << query.id
                  << ")\n"
                  << "            type=" << type_name(query.type)
                  << "  security=" << security_name(query.security) << "\n"
                  << "            arguments: " << query.arguments.dump() << "\n"
                  << "  approve? [y] once / [n] no / [a] always this tool / "
                     "[A] always everything / [d] deny the rest: "
                  << std::flush;

        std::string answer;
        if (!std::getline(std::cin, answer)) {
            // End of input: there is nobody left to ask, and asking again
            // would only ever fail again.
            _deny_all = true;
            std::cout << "\n";
            event.decision = tools::ConfirmDecision::Denied;
            event.reason = "the terminal reached end of input, so no "
                           "confirmation can be answered";
            return event;
        }

        const char choice = answer.empty() ? 'n' : answer.front();
        switch (choice) {
            case 'y':
            case 'Y':
                event.decision = tools::ConfirmDecision::Approved;
                event.reason = "approved at the terminal";
                break;
            case 'a':
                _always_allowed.insert(query.name);
                std::cout << "  [confirm] " << query.name
                          << " will not be asked about again this session\n";
                event.decision = tools::ConfirmDecision::Approved;
                event.reason = "approved at the terminal, for this tool, for "
                               "the rest of the session";
                break;
            case 'A':
                // Every tool, including the ones this set has not offered yet.
                _assume_yes = true;
                event.decision = tools::ConfirmDecision::Approved;
                event.reason = "approved at the terminal, for every tool, for "
                               "the rest of the session";
                break;
            case 'd':
            case 'D':
                _deny_all = true;
                event.decision = tools::ConfirmDecision::Denied;
                event.reason = "the session was told to deny the remaining "
                               "confirmations";
                break;
            default:
                // Anything else — `n` included — is a refusal, which is the
                // safe reading of an answer this prompt did not offer.
                event.decision = tools::ConfirmDecision::Denied;
                event.reason = "refused at the terminal";
                break;
        }
        return event;
    }

    std::mutex _mutex;
    /// At least one confirming tool was approved for the rest of the session.
    std::set<std::string, std::less<>> _always_allowed;
    /// --yes, `A`, or "stdin is gone".
    bool _assume_yes = false;
    /// `d`: refuse every confirmation from here on.
    bool _deny_all = false;
};

// ---- one user turn: the ReAct agent loop --------------------------------------

/// One record as the terminal shows it: the SETTLED call (which is what the
/// registry answers with — defaults filled in, type and security written by the
/// tool), the failure stage when the record is a failure, and the payload the
/// model is about to read.
void report_record(const model_io::InvokeReturn& record) {
    std::cout << "  [tool] " << record.query.id << " " << record.query.name
              << " " << record.query.arguments.dump() << " — settled "
              << type_name(record.query.type) << "/"
              << security_name(record.query.security) << "\n";

    if (const std::optional<tools::InvokeException::Stage> stage =
            tools::error_stage(record)) {
        // is_error()/error_stage() read the marker the failure record carries
        // in extras; the model itself only ever sees output.raw.
        std::cout << "         FAILED (" << tools::InvokeException::stage_key(*stage)
                  << "): " << record.output.raw << "\n";
        return;
    }
    std::cout << "         -> " << record.output.raw << "\n";
}

/// Run the agent loop for the last user turn through the LLMModel contract and
/// the tool registry: converse() for one exchange, integrate() to fold every
/// item into the growing AgentInputState, and — when the response carries
/// calls — ONE ToolRegistry::execute() batch, whose records go back into the
/// conversation as InvokeReturn items. A response with no invokes is the final
/// answer.
///
/// `max_steps` is the turn's exchange budget (--max-steps), and running out of
/// it is not a failure: every call the model made has already run and its
/// result is already in the conversation, so the next user message continues
/// from there — which is what the message on stderr says, because a terminal
/// that just stops answering reads like a crash otherwise.
asio::awaitable<void> run_turn(llm::LLMModel& model,
                               const tools::ToolRegistry& registry,
                               model_io::AgentInputState& state,
                               std::size_t max_steps) {
    for (std::size_t step = 0; step < max_steps; ++step) {
        model_io::MessageItem item = co_await model.converse(state);
        model.integrate(state, item);

        // Reasoning already streamed live through the bus subscription in
        // main(); the visible answer prints here, per exchange.
        for (const model_io::Content& part : item.content) {
            if (!part.raw.empty()) std::cout << part.raw << "\n";
        }

        if (!item.invokes || item.invokes->empty()) {
            // Final answer. Report the exchange's token accounting (the
            // cache_hit field is DeepSeek's prompt_cache_hit_tokens bridged by
            // the dialect).
            if (item.cost) {
                std::cerr << "[cost] prompt=" << item.cost->prompt
                          << " generated=" << item.cost->generated
                          << " cache_hit=" << item.cost->cache_hit << "\n";
            }
            co_return;
        }

        // The calls as they arrived — the raw form, before anything settled
        // them. What runs is the settled form, which the records below carry.
        std::vector<model_io::InvokeQuery> batch = *item.invokes;
        for (const model_io::InvokeQuery& call : batch) {
            std::cout << "  [call] " << call.id << " " << call.name << " "
                      << call.arguments.dump() << "\n";
        }

        // Steps 1-4 in one await: the registry settles the whole batch, runs
        // the serial calls one at a time, overlaps the ReadOnly ones, and
        // answers with one record per call in call order. The convenience
        // overload takes the executor from this coroutine — the io context the
        // store and the session strands live on.
        std::vector<model_io::InvokeReturn> records =
            co_await registry.execute(std::move(batch));

        for (const model_io::InvokeReturn& record : records) {
            report_record(record);

            // The record IS the tool result: the payload the model reads
            // (output), plus the settled query whose id the next request
            // correlates it by (invoke_return). Nothing here rebuilds or
            // annotates it.
            model_io::MessageItem result;
            result.type = model_io::MessageItemType::InvokeReturn;
            result.role = "tool";
            result.content.push_back(record.output);
            result.invoke_return = record;
            model.integrate(state, result);
        }
    }
    std::cerr << "agent loop hit its " << max_steps
              << "-step budget before a final answer.\n"
              << "  Every tool call it made has already run, and the results "
                 "are in the conversation:\n"
              << "  send another message (\"continue\" is enough) and it picks "
                 "up from there, or relaunch with a larger --max-steps.\n";
}

// ---- the host's own view of the session table ---------------------------------

/// `/sessions`: what the table holds right now, read through the store the
/// tools share — the host-side view the toolset's README describes, next to
/// (and not instead of) what the model is told through poll_processes.
asio::awaitable<void> print_sessions(
    const std::shared_ptr<tools::intrinsic::ProcessSessionStore>& store) {
    const std::vector<tools::intrinsic::SessionSnapshot> snapshots =
        co_await store->snapshots();
    if (snapshots.empty()) {
        std::cout << "no sessions\n";
        co_return;
    }
    for (const tools::intrinsic::SessionSnapshot& snapshot : snapshots) {
        std::cout << "  " << snapshot.id
                  << "  pid=" << snapshot.result.spec.pid
                  << "  " << snapshot.result.spec.executable
                  << "  state="
                  << nlohmann::json(snapshot.result.execution.state)
                         .get<std::string>()
                  << (snapshot.exited ? " (observed exited)" : "")
                  << (snapshot.output_drained ? "" : " (output still draining)")
                  << "\n";
    }
    std::cout << "retained sessions: " << co_await store->size() << " / "
              << tools::intrinsic::ProcessSessionStore::kDefaultMaxSessions
              << " (an exited session keeps its slot until it is released)\n";
}

} // namespace

int main(int argc, char* argv[]) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage(argv[0]);
        return 2;
    }
    if (options.help) {
        print_usage(argv[0]);
        return 0;
    }

    std::cout << "=== DeepSeek chat over the tools stack (LLM plugin + "
                 "ToolRegistry + intrinsic process toolset) ===\n";

    // ---- the tool side, wired before anything else --------------------------
    //
    // Declaration order IS the shutdown order (destruction runs in reverse):
    // the registry holds the set, the set holds the tools, the tools hold the
    // store. So the store outlives them all, which is what lets the shutdown
    // tail below terminate children before the table goes away.
    asio::io_context io;
    auto store = std::make_shared<tools::intrinsic::ProcessSessionStore>(
        io.get_executor());
    // nullptr bus = the process-wide eventbus::default_async_bus(), which is
    // where the TerminalConfirmer below subscribes. One bus per process: the
    // toolset and the confirmer are different modules and still meet.
    auto process_set = std::make_shared<tools::intrinsic::ProcessToolSet>(store);

    tools::ToolRegistry registry;
    registry.add(process_set);

    if (options.catalogue_only) {
        return report_catalogue(registry, *process_set);
    }

    // ---- the provider side ---------------------------------------------------
    std::string api_key;
    std::cout << "api_key [env DEEPSEEK_API_KEY]: ";
    if (!std::getline(std::cin, api_key) || api_key.empty()) {
        if (const char* env = std::getenv("DEEPSEEK_API_KEY")) {
            api_key = env;
        }
    }
    if (api_key.empty()) {
        std::cerr << "no api_key provided; exiting.\n";
        return 1;
    }

    std::string effort = "high";
    if (!options.list_models_only) {
        std::cout << "reasoning effort [high] (none|minimal|low|medium|high|"
                     "xhigh|max; none/minimal disable thinking): ";
        if (std::string line; std::getline(std::cin, line) && !line.empty()) {
            effort = line;
        }
    }

    // Load the provider plugins the build emits next to this executable. The
    // registry is then a concurrent-read router: provider name -> factory.
    llm::LLMDispatcher dispatcher;
    const std::size_t loaded = dispatcher.load_default_models();
    if (loaded == 0) {
        std::cerr << "no provider plugins under <exe>/plugins/llm; build the "
                     "llm_deepseek target first.\n";
        return 1;
    }
    std::cout << "loaded " << loaded << " provider plugin(s)\n";

    const nlohmann::json config{
        {"model", model_name()},
        {"reasoning", {{"effort", effort}}},
        // The dialect's endpoint defaults apply underneath: base_url
        // https://api.deepseek.com, /chat/completions, Bearer — only the
        // credential needs supplying. Retry stays at the adapter defaults
        // (3 retries, 500ms..120s backoff).
        {"endpoint", {{"auth", {{"api_key", api_key}}}}},
    };
    auto model = dispatcher.create_model("deepseek", io.get_executor(), config);
    if (!model) {
        std::cerr << "provider \"deepseek\" did not load; is the plugin "
                     "built and ABI-matched?\n";
        return 1;
    }

    // The catalogue mode: the provider's live model list (plus the balance
    // companion the DeepSeek dialect attaches) over the same endpoint/auth,
    // then exit — no conversation, no tool.
    if (options.list_models_only) {
        try {
            std::size_t count = 0;
            auto future = asio::co_spawn(
                io,
                [&]() -> asio::awaitable<void> {
                    nlohmann::json catalogue = co_await model->provider_info();
                    // Array = the bare models list; an object carries the
                    // models under "models" with provider extras (the
                    // balance companion) as siblings.
                    const nlohmann::json models =
                        catalogue.is_array()
                            ? catalogue
                            : catalogue.value(
                                  "models", nlohmann::json::array());
                    count = models.size();
                    for (const auto& entry : models) {
                        std::cout << "  " << entry.dump() << "\n";
                    }
                    if (catalogue.is_object() &&
                        catalogue.contains("balance")) {
                        std::cout << "balance:\n"
                                  << catalogue["balance"].dump(2) << "\n";
                    }
                },
                asio::use_future);
            io.run();
            future.get();
            std::cout << count << " model(s) offered by deepseek\n";
        } catch (const std::exception& error) {
            // Base catch only: the chain runs inside the provider .so (same
            // rationale as the turn loop below).
            std::cerr << "provider_info failed: " << error.what() << "\n";
            return 1;
        }
        return 0;
    }

    // ---- the conversation half ----------------------------------------------
    //
    // The tools the model is offered are the registry's flattened catalogue —
    // not a hand-built list — so what the model sees is exactly what execute()
    // can route. A set that registered nothing would leave `tools` empty and
    // the model simply has no calls to make; the banner below says so either
    // way rather than this file guessing.
    model_io::AgentInputState state;
    state.system_prompt.add_section(
        "persona", "",
        "You are a helpful assistant running on a Linux host, with a "
        "process-management toolset. Use spawn_process to run programs: an "
        "ordinary command returns its exit code and output in that one call, "
        "while a program that outlives the wait comes back as a session id "
        "(proc_N) to follow with poll_processes, read_process_output, "
        "wait_process, write_process_input and kill_process. Sessions and "
        "their output survive across turns until they are released. There is "
        "no shell: pass the program and its arguments separately, or run "
        "'sh' with '-c' explicitly. Every state-changing call is confirmed by "
        "the person at the terminal before it runs, so ask for what you need "
        "directly and keep commands small. You have " +
            std::to_string(options.max_steps) +
            " tool-call rounds per message: prefer one decisive call to a "
            "poll loop (wait_process with a real timeout instead of asking "
            "again and again), and if a task needs more rounds than that, say "
            "where you got to so the person can answer \"continue\".",
        model_io::SectionStability::Immutable);
    state.tools = registry.get_tools();

    std::cout << "model: " << model_name() << " (thinking "
              << (effort == "none" || effort == "minimal" ? "disabled"
                                                          : "enabled")
              << ", effort " << effort << ")\n";
    std::cout << "tools registered: " << state.tools.size() << " from "
              << registry.size() << " set(s) —";
    for (const model_io::Invocable& tool : state.tools) {
        std::cout << " " << tool.name;
    }
    std::cout << "\n";
    std::cout << "spawn_process / write_process_input / kill_process ask at "
                 "the terminal before they run";
    if (options.assume_yes) {
        std::cout << " (--yes: auto-approved)";
    }
    std::cout << ".\n";
    std::cout << "budget: " << options.max_steps
              << " tool-call rounds per message (--max-steps N to change).\n";
    std::cout << "commands: /tools /sessions /help; empty line to quit.\n";
    std::cout << "try: \"run seq 1 5 and show me the output\", or \"start cat, "
                 "feed it hello, then read back what it printed\".\n";
    std::cout << "reasoning streams live to stderr; tool results print as they "
                 "come back.\n";

    // The confirmation gate: the one authority that can approve a
    // RequireConfirm call on this host. Subscribed process-wide, because that
    // is where security_check.hpp publishes.
    TerminalConfirmer confirmer(options.assume_yes);
    eventbus::AsyncEventBus::ScopedSubscription confirmations =
        confirmer.attach(eventbus::default_async_bus());

    // The live view: every provider's reasoning increments, mirrored to
    // stderr as they stream. Synchronous bus — the slot runs inline on the
    // exchange's I/O thread in wire order (and must not throw).
    eventbus::EventBus::ScopedSubscription reasoning_view =
        eventbus::default_bus()
            .subscribe<llm::chat_completions::ReasoningDeltaEvent>(
                [](const llm::chat_completions::ReasoningDeltaEvent& event) {
                    std::cerr << event.reasoning << std::flush;
                });

    std::string line;
    while (std::cout << "\nyou> " && std::getline(std::cin, line)) {
        if (line.empty()) break;

        if (line == "/help") {
            std::cout << "commands:\n"
                      << "  /tools     the catalogue the model was given\n"
                      << "  /sessions  the sessions the store is holding\n"
                      << "  /help      this\n"
                      << "  (empty)    quit\n";
            continue;
        }
        if (line == "/tools") {
            report_catalogue(registry, *process_set);
            continue;
        }
        if (line == "/sessions") {
            try {
                auto future = asio::co_spawn(io, print_sessions(store),
                                             asio::use_future);
                io.restart();   // a prior turn's run() drained the context
                io.run();
                future.get();
            } catch (const std::exception& error) {
                std::cerr << "sessions failed: " << error.what() << "\n";
            }
            continue;
        }

        model_io::MessageItem input;
        input.type = model_io::MessageItemType::UserInput;
        input.role = "user";
        model_io::Content text;
        text.type = model_io::ContentType::Text;
        text.raw = line;
        input.content.push_back(std::move(text));
        model->integrate(state, input);

        try {
            auto future = asio::co_spawn(
                io, run_turn(*model, registry, state, options.max_steps),
                asio::use_future);
            io.restart();   // a prior turn's run() drained the context
            io.run();
            future.get();
        } catch (const std::exception& error) {
            // Deliberately the base catch only: converse() runs inside the
            // provider .so, and its adapter types (e.g. the API exception)
            // carry one typeinfo per module — exact-type catches across the
            // dlopen boundary are not dependable. what() already carries
            // the API's own diagnosis when the exchange reached the server.
            std::cerr << "turn failed: " << error.what() << "\n";
            state.turns.pop_back();   // drop the unanswered turn
        }
    }

    // ---- shutdown ------------------------------------------------------------
    //
    // Children first, and while the context still runs: terminate_all() is a
    // coroutine (a destructor has no executor to run one on), and dropping the
    // table does not stop a child — each handle's await task holds a reference
    // until the terminal state is observed (process/session_store.hpp). The
    // context is single-threaded and drained here, so it is quiesced by the
    // time run() returns, which is the other half of the contract: the table
    // may only be freed once nothing is queued on the context it shares.
    try {
        auto future = asio::co_spawn(io, store->terminate_all(false),
                                     asio::use_future);
        io.restart();
        io.run();
        const std::size_t signalled = future.get();
        if (signalled != 0) {
            std::cout << "\nshutdown: signalled " << signalled
                      << " live child process(es)\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "shutdown: terminate_all failed: " << error.what()
                  << " (the store's destructor will signal recorded pids)\n";
    }

    // The registry holds the only set, which holds the tools, which hold the
    // store: clearing it is what lets the store actually be destroyed below.
    confirmations.disconnect();
    registry.clear();
    process_set.reset();
    store.reset();
    return 0;
}
