/**
 * Interactive DeepSeek/process-tool host using loop::run, not a second loop.
 * All UI and coroutine continuations run on one io_context thread. Async stdin
 * leaves that thread free to drain child pipes while a human considers a prompt.
 * Run this example in the disposable test image; it executes real commands.
 */
#include "terminal.hpp"
#include "loop/loop.hpp"
#include "loop/events.hpp"
#include "llm/models.hpp"
#include "logging/logger.hpp"
#include "eventbus/async_event_bus.hpp"
#include "eventbus/event_bus.hpp"
#include "tools/intrinsic/process/toolset.hpp"
#include "tools/intrinsic/process/tools.hpp"
#include "tools/invoke_exception.hpp"
#include "tools/registry.hpp"
#include "tools/security_check.hpp"

#include <boost/asio.hpp>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <stop_token>
#include <unistd.h>

namespace asio = boost::asio;
using loop_example::block;

namespace {

/** Host options; model and credentials remain runtime configuration. */
struct Options {
    bool tools = false;
    bool skill = false;
    bool list_models = false;
    bool yes = false;
    bool reasoning = false;
    bool help = false;
    std::size_t max_steps = 12;
    std::string effort = "high";
    std::string log = "/tmp/loop-deepseek-chat.log";
};

/** Prints both command-line options and the commands available between runs. */
void usage() {
    std::cout <<
        "loop_deepseek_chat [--tools] [--skill] [--list-models] [--yes]\n"
        "  [--max-steps N] [--reasoning] [--effort high] [--log PATH]\n"
        "Environment: DEEPSEEK_API_KEY, DEEPSEEK_MODEL (default deepseek-v4-flash),\n"
        "  DEEPSEEK_BASE_URL (optional compatible endpoint).\n"
        "Commands: /tools /skill /sessions /state /continue /help /quit\n"
        "Empty input quits. Ctrl-C stops a run; at the input prompt it quits.\n"
        "--reasoning shows complete reasoning blocks, never interleaved deltas.\n"
        "--yes approves tool calls; otherwise confirmation is required.\n";
}

/** Strict flag parsing: an invalid approval flag must never silently succeed. */
Options parse(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        std::string flag = argv[index];
        auto value = [&]() -> std::string {
            if (++index == argc) {
                throw std::invalid_argument("missing value for " + flag);
            }
            return argv[index];
        };
        if (flag == "--tools") options.tools = true;
        else if (flag == "--skill") options.skill = true;
        else if (flag == "--list-models") options.list_models = true;
        else if (flag == "--yes" || flag == "-y") options.yes = true;
        else if (flag == "--reasoning") options.reasoning = true;
        else if (flag == "--help" || flag == "-h") options.help = true;
        else if (flag == "--effort") options.effort = value();
        else if (flag == "--log") options.log = value();
        else if (flag == "--max-steps" || flag.starts_with("--max-steps=")) {
            const auto text = flag == "--max-steps" ? value() : flag.substr(12);
            const auto [end, error] = std::from_chars(
                text.data(), text.data() + text.size(), options.max_steps);
            if (error != std::errc{} || end != text.data() + text.size() || options.max_steps == 0) {
                throw std::invalid_argument("--max-steps requires a positive integer");
            }
        } else {
            throw std::invalid_argument("unknown option: " + flag);
        }
    }
    return options;
}

/**
 * One outstanding async line read shared by the REPL and tool confirmer.
 * cancel() only interrupts terminal input, never registry execution. Ctrl-C at
 * a confirmation therefore denies that call while the loop settles its batch.
 */
class Input {
public:
    explicit Input(asio::io_context& io) : descriptor(io, duplicate_stdin()) {}

    asio::awaitable<std::optional<std::string>> read(std::string_view prompt) {
        std::cout << prompt << std::flush;
        boost::system::error_code error;
        const auto count = co_await asio::async_read_until(
            descriptor, asio::dynamic_buffer(buffer), '\n',
            asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            if (error == asio::error::eof && !buffer.empty()) {
                co_return std::exchange(buffer, {});
            }
            buffer.clear();
            if (error != asio::error::eof && error != asio::error::operation_aborted) {
                throw boost::system::system_error(error);
            }
            co_return std::nullopt;
        }
        std::string line = buffer.substr(0, count - 1);
        buffer.erase(0, count);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        co_return line;
    }

    void cancel() {
        boost::system::error_code ignored;
        descriptor.cancel(ignored);
    }

private:
    static int duplicate_stdin() {
        const int fd = ::dup(STDIN_FILENO);
        if (fd < 0) throw std::runtime_error("cannot duplicate stdin");
        return fd;
    }
    asio::posix::stream_descriptor descriptor;
    std::string buffer;
};

/**
 * Owns signal callback state beyond the chat frame. close() cancels the pending
 * handler; its shared ownership makes a delayed aborted completion harmless.
 * Input belongs to main and outlives io.run(), including all signal completions.
 */
class StopControl : public std::enable_shared_from_this<StopControl> {
public:
    StopControl(asio::io_context& io, Input& input)
        : signals(io, SIGINT, SIGTERM), input(input) {}

    void arm() {
        signals.async_wait([self = shared_from_this()](const boost::system::error_code& error, int signal) {
            if (error || self->closed) return;
            if (signal == SIGTERM || !self->active) self->quitting = true;
            if (self->active) {
                self->active->request_stop();
                self->stopped = true;
            }
            self->input.cancel();
            std::cout << "\n[stop] "
                      << (self->active ? "requested; settling active work" : "leaving chat")
                      << '\n' << std::flush;
            self->arm();
        });
    }

    void close() {
        closed = true;
        signals.cancel();
    }

    std::optional<std::stop_source> active;
    bool quitting = false;
    bool stopped = false;

private:
    asio::signal_set signals;
    Input& input;
    bool closed = false;
};

/** Approval decisions are serialized by the process toolset's serial writes. */
class Confirmer {
public:
    Confirmer(Input& input, bool yes, const bool& stopped)
        : input(input), yes(yes), stopped(stopped) {}

    asio::awaitable<tools::InvokeConfirmEvent> decide(tools::InvokeConfirmEvent event) {
        if (deny || stopped) {
            event.decision = tools::ConfirmDecision::Denied;
            event.reason = "confirmation denied or run stopped";
            co_return event;
        }
        if (yes || allowed.contains(event.query.name)) {
            event.decision = tools::ConfirmDecision::Approved;
            event.reason = "approved by session policy";
            co_return event;
        }
        block(std::cout, "Confirm " + event.query.name + " [" + event.query.id + "]",
              event.query.arguments.dump(2));
        const auto answer = co_await input.read(
            "approve [y] once / [n] deny / [a] this tool always / [A] all / [d] deny all: ");
        const auto choice = answer && !answer->empty() ? answer->front() : 'n';
        if (!stopped && (choice == 'y' || choice == 'Y' || choice == 'a' || choice == 'A')) {
            if (choice == 'a') allowed.insert(event.query.name);
            if (choice == 'A') yes = true;
            event.decision = tools::ConfirmDecision::Approved;
            event.reason = "approved at the terminal";
        } else {
            if (choice == 'd') deny = true;
            event.decision = tools::ConfirmDecision::Denied;
            event.reason = "refused at the terminal";
        }
        co_return event;
    }

private:
    Input& input;
    bool yes;
    const bool& stopped;
    bool deny = false;
    std::set<std::string> allowed;
};

/** Checks the exact five routed tools and skill injection without starting a child. */
int catalogue(const tools::ToolRegistry& registry) {
    for (const auto& tool : registry.get_tools()) {
        block(std::cout, tool.name, tool.description);
    }
    for (const auto name : {
             tools::intrinsic::tool_names::kSpawn, tools::intrinsic::tool_names::kRun,
             tools::intrinsic::tool_names::kPoll, tools::intrinsic::tool_names::kRead,
             tools::intrinsic::tool_names::kSend}) {
        if (!registry.contains(name)) throw std::runtime_error("process catalogue is incomplete");
    }
    model_io::PromptTemplate prompt;
    if (registry.inject_skills(prompt) != 1 || prompt.render().markdown.empty()) {
        throw std::runtime_error("process skill did not load/inject");
    }
    return 0;
}

/** Shows the toolset-owned guidance; no locally duplicated skill prose. */
void skill(const tools::ToolRegistry& registry) {
    for (const auto& set : registry.get_registered()) {
        const auto guidance = set->skill();
        if (!guidance) throw std::runtime_error("process skill is missing");
        block(std::cout, guidance->name, guidance->text);
    }
}

/** Host inspection does not consume either process output cursor. */
asio::awaitable<void> sessions(tools::intrinsic::ProcessSessionStore& store) {
    for (const auto& snapshot : co_await store.snapshots()) {
        const nlohmann::json state = snapshot.result.execution.state;
        block(std::cout, snapshot.id,
              "pid=" + std::to_string(snapshot.result.spec.pid) +
              " state=" + state.get<std::string>() +
              " output_drained=" + (snapshot.output_drained ? "true" : "false"));
    }
    std::cout << "retained sessions: " << co_await store.size() << '\n';
}

/** Maps the non-persistent run summary to a short terminal label. */
const char* status_name(loop::RunStatus status) {
    switch (status) {
        case loop::RunStatus::Completed: return "completed";
        case loop::RunStatus::Cancelled: return "cancelled";
        case loop::RunStatus::StepLimit: return "step limit";
        case loop::RunStatus::Failed: return "failed";
    }
    return "unknown";
}

/**
 * Runs the interactive host. The only agent execution is loop::run below;
 * subscriptions merely render already committed data. No hooks prune history by
 * default: the transcript remains available for /continue and recovery inspection.
 */
asio::awaitable<void> chat(
    asio::io_context& io, Input& input, const Options& options,
    tools::ToolRegistry& registry, tools::intrinsic::ProcessSessionStore& store) {
    std::string key;
    if (const auto env = std::getenv("DEEPSEEK_API_KEY")) key = env;
    if (key.empty()) {
        auto answer = co_await input.read("API key (or set DEEPSEEK_API_KEY): ");
        if (answer) key = std::move(*answer);
    }
    if (key.empty()) throw std::runtime_error("no API key supplied");
    const auto configured_model = std::getenv("DEEPSEEK_MODEL");
    const std::string model_name = configured_model ? configured_model : "deepseek-v4-flash";
    llm::LLMDispatcher dispatcher;
    dispatcher.load_default_models();
    nlohmann::json config{
        {"model", model_name}, {"reasoning", {{"effort", options.effort}}},
        {"endpoint", {{"auth", {{"api_key", key}}}}}
    };
    if (const auto base_url = std::getenv("DEEPSEEK_BASE_URL")) {
        config["endpoint"]["base_url"] = base_url;
    }
    auto model = dispatcher.create_model("deepseek", io.get_executor(), config);
    if (!model) throw std::runtime_error("DeepSeek plugin missing or ABI mismatch");
    if (options.list_models) {
        block(std::cout, "Provider catalogue", (co_await model->provider_info()).dump(2));
        co_return;
    }

    model_io::AgentInputState state;
    state.system_prompt.add_section("persona", "",
        "You are a helpful assistant in a disposable Linux container. Use the process "
        "tools for commands and interactive programs. Follow the injected process skill. "
        "Tool calls require human approval unless the host has enabled auto-approval. "
        "Prefer a bounded wait to repeated polling. Report facts and distinguish stdout "
        "from stderr. If the exchange budget ends, the human can use /continue.",
        model_io::SectionStability::Immutable);
    registry.inject_skills(state.system_prompt);
    state.tools = registry.get_tools();
    eventbus::EventBus events;
    auto control = std::make_shared<StopControl>(io, input);
    Confirmer confirmer(input, options.yes, control->stopped);
    eventbus::AsyncEventBus::ScopedSubscription confirmation =
        eventbus::default_async_bus().subscribe<tools::InvokeConfirmEvent>(
            [&](tools::InvokeConfirmEvent event) -> asio::awaitable<tools::InvokeConfirmEvent> {
                co_return co_await confirmer.decide(std::move(event));
            });
    auto before_model = events.subscribe<loop::BeforeModel>([](const auto&) {
        std::cout << "\n[model] waiting for a complete response; Ctrl-C to stop...\n" << std::flush;
    });
    auto response = events.subscribe<loop::ModelCommitted>([&](const auto& event) {
        const auto& item = event.state.turns.back().agent_loop_step.back().model_response;
        if (options.reasoning && item.reasoning) block(std::cout, "Reasoning", item.reasoning->raw);
        for (const auto& part : item.content) block(std::cout, "Assistant", part.raw);
        if (item.cost) {
            std::cout << "[tokens] prompt=" << item.cost->prompt
                      << " generated=" << item.cost->generated
                      << " cache_hit=" << item.cost->cache_hit << '\n';
        }
    });
    auto calls = events.subscribe<loop::BeforeToolBatch>([](const auto& event) {
        for (const auto& call : event.calls) {
            block(std::cout, "Call " + call.name + " [" + call.id + "]", call.arguments.dump(2));
        }
    });
    auto results = events.subscribe<loop::ToolResultsCommitted>([](const auto& event) {
        const auto& step = event.state.turns.back().agent_loop_step.back();
        if (!step.invoke_returns) return;
        for (const auto& item : *step.invoke_returns) {
            const auto& result = *item.invoke_return;
            block(std::cout, std::string(tools::is_error(result) ? "Tool FAILED " : "Tool result ") +
                  result.query.name + " [" + result.query.id + "]", result.output.raw);
        }
    });
    auto finished = events.subscribe<loop::RunFinished>([](const auto& event) {
        block(std::cout, "Run " + std::string(status_name(event.result.status)),
              "exchanges=" + std::to_string(event.result.completed_exchanges) +
              " phase=" + nlohmann::json(event.state.loop->phase).template get<std::string>() +
              (event.result.error.empty() ? "" : "\n" + event.result.error));
    });

    control->arm();
    std::exception_ptr failure;
    try {
        block(std::cout, "Loop chat", "model=" + model_name + "\nUse /help for commands.");
        while (!control->quitting) {
            auto line = co_await input.read("\nyou> ");
            if (!line || line->empty() || *line == "/quit") break;
            if (*line == "/help") { usage(); continue; }
            if (*line == "/tools") { catalogue(registry); continue; }
            if (*line == "/skill") { skill(registry); continue; }
            if (*line == "/sessions") { co_await sessions(store); continue; }
            if (*line == "/state") {
                block(std::cout, "Loop progress", state.loop ? nlohmann::json(*state.loop).dump(2) : "not started");
                continue;
            }
            const bool has_message = *line != "/continue";
            model_io::MessageItem message;
            message.role = "user";
            if (has_message) {
                model_io::Content content;
                content.raw = std::move(*line);
                message.content.push_back(std::move(content));
            }
            control->active.emplace();
            control->stopped = false;
            try {
                const auto result = co_await loop::run(
                    *model, registry, events, io.get_executor(), state, has_message,
                    std::move(message), {options.max_steps}, control->active->get_token());
                if (!result.error.empty()) {
                    block(std::cout, "Continuation", "History retained. Inspect /state and /sessions; "
                          "use /continue to retry only when recovery permits.\n" + result.error);
                }
                if (result.status == loop::RunStatus::StepLimit) {
                    std::cout << "Budget exhausted; /continue resumes without adding user input.\n";
                }
            } catch (const std::exception& error) {
                block(std::cout, "Host exception", error.what());
            }
            control->active.reset();
        }
    } catch (...) {
        failure = std::current_exception();
    }
    control->close();
    if (failure) std::rethrow_exception(failure);
}

} // namespace

/** Owns services until the asynchronous host and process cleanup have drained. */
int main(int argc, char** argv) {
    try {
        const auto options = parse(argc, argv);
        if (options.help) { usage(); return 0; }
        std::ofstream log(options.log, std::ios::app);
        if (!log) throw std::runtime_error("cannot open diagnostic log: " + options.log);
        // Only framework errors use stderr. The interactive transcript has one
        // writer/stream, so a partial reasoning line can never split a prompt.
        struct RestoreBuffer {
            std::streambuf* previous;
            ~RestoreBuffer() { std::cerr.rdbuf(previous); }
        } restore{std::cerr.rdbuf(log.rdbuf())};
        logging::Logger::set_level(logging::LogLevel::error);
        asio::io_context io;
        auto store = std::make_shared<tools::intrinsic::ProcessSessionStore>(io.get_executor());
        auto process_set = std::make_shared<tools::intrinsic::ProcessToolSet>(store);
        tools::ToolRegistry registry;
        registry.add(process_set);
        if (options.tools) return catalogue(registry);
        if (options.skill) { skill(registry); return 0; }
        std::cout << "Framework error log: " << options.log << '\n';
        Input input(io);
        auto host = [&]() -> asio::awaitable<int> {
            int status = 0;
            try {
                co_await chat(io, input, options, registry, *store);
            } catch (const std::exception& error) {
                block(std::cout, "Host failure", error.what());
                status = 1;
            }
            const auto signalled = co_await store->terminate_all(false);
            std::cout << "Shutdown: signalled " << signalled << " live child process(es).\n";
            co_return status;
        };
        auto future = asio::co_spawn(io, host, asio::use_future);
        io.run();
        return future.get();
    } catch (const std::exception& error) {
        block(std::cout, "Error", error.what());
        return 1;
    }
}
