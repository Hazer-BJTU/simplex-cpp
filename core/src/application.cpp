#include "core/application.hpp"
#include "fileio/session_lock.hpp"
#include "core/confirmation.hpp"
#include "core/protocol.hpp"
#include "load/plugins.hpp"
#include "load/persistence.hpp"
#include "loop/loop.hpp"
#include "loop/events.hpp"
#include "loop/hook_registry.hpp"
#include "loop/intrinsic/context_statistic/hook.hpp"
#include "tools/intrinsic/process/toolset.hpp"
#include "tools/registry.hpp"
#include <boost/asio/experimental/channel.hpp>
#include <unordered_set>
#include <deque>
#include <iostream>
#include <ctime>

namespace core {
namespace asio = boost::asio;
using Json = nlohmann::json;

namespace {
/** Wall-clock metadata only; deadlines and ordering use other mechanisms. */
std::string timestamp() {
    const auto now = std::time(nullptr);
    std::tm utc{};
    char text[32]{};
    if (!::gmtime_r(&now, &utc)
        || std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        throw std::runtime_error("cannot format session timestamp");
    }
    return text;
}

/** Queue cancellation must not hide the transport error that closed it. */
bool channel_shutdown(std::exception_ptr failure) {
    if (!failure) return false;
    try {
        std::rethrow_exception(failure);
    } catch (const boost::system::system_error& error) {
        return error.code() == asio::experimental::channel_errc::channel_closed
            || error.code() == asio::experimental::channel_errc::channel_cancelled
            || error.code() == asio::error::operation_aborted;
    } catch (...) {
        return false;
    }
}

/** Internal bookkeeping stays typed; wire labels remain protocol-compatible. */
enum class SaveBoundary {
    BeforeTools,
    ResultsReady,
    StepFinished,
    RunFinished,
    Cancelled,
    Shutdown
};

const char* boundary_name(SaveBoundary boundary) {
    switch (boundary) {
        case SaveBoundary::BeforeTools: return "before_tools";
        case SaveBoundary::ResultsReady: return "results_ready";
        case SaveBoundary::StepFinished: return "step_finished";
        case SaveBoundary::RunFinished: return "run_finished";
        case SaveBoundary::Cancelled: return "cancelled";
        case SaveBoundary::Shutdown: return "shutdown";
    }
    throw std::logic_error("invalid persistence boundary");
}

std::string run_status(loop::RunStatus status) {
    switch (status) {
        case loop::RunStatus::Completed: return "completed";
        case loop::RunStatus::Cancelled: return "cancelled";
        case loop::RunStatus::ExchangeLimit: return "exchange_limit";
        case loop::RunStatus::Failed: return "failed";
    }
    throw std::invalid_argument("invalid run status");
}
}


/** Strand-owned runtime. Only run control and the single-use latch cross threads. */
struct Application::Impl : std::enable_shared_from_this<Impl> {
    using Queue = asio::experimental::channel<void(boost::system::error_code, Json)>;
    using Done = asio::experimental::channel<void(boost::system::error_code, bool)>;

    Impl(asio::any_io_executor executor, load::Configuration configuration,
         std::string session, std::shared_ptr<llm::LLMModel> injected)
        : strand(asio::make_strand(executor)), config(std::move(configuration)),
          session_id(std::move(session)), model(std::move(injected)),
          hooks(events), store(std::make_shared<tools::intrinsic::ProcessSessionStore>(strand)),
          client(strand, config.client, events, config.queues, config.transport),
          outgoing(strand, config.event_capacity), sender_done(strand, 1), client_done(strand, 1) {
        validate_session_id(session_id);
        if (config.event_capacity == 0) throw std::invalid_argument("event_capacity must be positive");
    }

    asio::strand<asio::any_io_executor> strand;
    load::Configuration config;
    std::string session_id;
    std::string worker_id = new_identity();
    std::shared_ptr<llm::LLMModel> model;
    eventbus::EventBus events;
    tools::ToolRegistry registry;
    loop::LoopHookRegistry hooks;
    std::shared_ptr<tools::intrinsic::ProcessSessionStore> store;
    io::Client client;
    Queue outgoing;
    Done sender_done;
    Done client_done;
    model_io::AgentInputState state;
    std::vector<eventbus::EventBus::ScopedSubscription> subscriptions;
    eventbus::AsyncEventBus::ScopedSubscription confirmation;
    ConfirmationOptions confirmation_options;
    std::unordered_set<std::string> requests;
    std::deque<std::string> request_order;
    std::atomic<bool> started{false};
    std::atomic<bool> shutdown_requested{false};
    std::mutex control_mutex;
    std::shared_ptr<ConfirmationScope> scope;
    std::stop_source run_stop;
    std::string run_id;
    std::string request_id;
    bool stopping = false;
    bool active = false;
    bool storage_failed = false;
    bool run_saved = false;
    std::exception_ptr failure;
    std::uint64_t sequence = 0;

    /** Close security admission before requesting loop cancellation. */
    void cancel(const std::string& expected = {}) {
        std::shared_ptr<ConfirmationScope> current;
        std::stop_source source;
        {
            std::lock_guard lock(control_mutex);
            if (!expected.empty() && expected != run_id) return;
            current = scope;
            source = run_stop;
        }
        if (current) current->cancel();
        source.request_stop();
    }

    void shutdown() {
        shutdown_requested.store(true);
        cancel();
        asio::post(strand, [self = shared_from_this()] {
            self->stopping = true;
            // A pending loop owns batch draining. Idle shutdown wakes next().
            if (!self->active) self->client.stop();
        });
    }

    /** Preserve the first failure while guaranteeing all supervisors wake. */
    void fail(std::exception_ptr error) {
        if (!failure) failure = error;
        stopping = true;
        shutdown_requested.store(true);
        cancel();
        client.stop();
        outgoing.close();
    }

    /** Copy only event data. Full queue is fatal, never an invisible drop. */
    void emit(std::string name, Json data = Json::object()) {
        Json message = {{"type", "event"}, {"event", std::move(name)},
            {"session_id", session_id}, {"worker_id", worker_id},
            {"request_id", request_id}, {"run_id", run_id},
            {"sequence", ++sequence}, {"data", std::move(data)}};
        if (!outgoing.try_send(boost::system::error_code{}, std::move(message))) {
            auto error = std::make_exception_ptr(std::runtime_error("application event queue exhausted"));
            fail(error);
            std::rethrow_exception(error);
        }
    }

    Json status() const {
        Json value = {{"active", active}, {"stopping", stopping},
            {"storage_failed", storage_failed}, {"rejected_payloads", client.rejected_payloads()}};
        if (state.loop) value["loop"] = *state.loop;
        return value;
    }

    /**
     * Read-only option discovery. Each category contains a list of advertised
     * choices, not active configuration. Empty reserved categories do not imply
     * that tools are disabled; their options are not exposed yet.
     */
    Json options() const {
        const llm::LLMModel& provider = *model;
        return {
            {"model", provider.get_options()},
            {"tools", Json::array()},
            {"confirmation", confirmation_options.get_options()}
        };
    }

    /** Required JSON saves latch failure even if RunFinished swallows observers. */
    void save(SaveBoundary boundary) {
        if (!config.persistence || storage_failed) return;
        const auto directory = config.storage / session_id;
        try {
            if (state.meta.session_id != session_id)
                throw std::logic_error("hook changed the worker session identity");
            load::save_state(directory / "state.json", state);
        } catch (...) {
            storage_failed = true;
            if (!failure) failure = std::current_exception();
            cancel();
            throw;
        }
        if (boundary == SaveBoundary::RunFinished
            || boundary == SaveBoundary::Cancelled) run_saved = true;
        emit("persisted", {{"boundary", boundary_name(boundary)}, {"format", "json"}});
        if (config.readable) {
            try {
                load::save_state(directory / "readable.md", state, load::StateFormat::Readable);
            } catch (const std::exception& error) {
                emit("export_error", {{"message", error.what()}});
            }
        }
    }

    /** Initialize resources before admission; restored history is never replayed. */
    void initialize() {
        construct_runtime();
        restore_state();
        install_confirmation();
        install_observers();
    }

    /** Construct registries before rebuilding their prompt representation. */
    void construct_runtime() {
        auto plugins = load::load_plugins(config.document, config.directory);
        auto& extensions = plugins.extensions;
        if (!model) model = plugins.providers.create_model(config.provider, strand, config.model);
        if (!model) throw std::runtime_error("driver provider could not construct a model");
        registry.add(std::make_shared<tools::intrinsic::ProcessToolSet>(
            store, &eventbus::default_async_bus()));
        for (auto& tool : extensions.tools) registry.add(std::move(tool));
        hooks.add(loop::intrinsic::ContextStatisticHook::from_config());
        for (auto& hook : extensions.loop_hooks) hooks.add(std::move(hook));
    }

    /** Restore history unchanged, reconciling only host-owned capabilities. */
    void restore_state() {
        const auto snapshot = config.storage / session_id / "state.json";
        const bool restored = config.persistence && config.restore && std::filesystem::exists(snapshot);
        if (restored) {
            state = load::load_state(snapshot);
            if (state.meta.session_id != session_id)
                throw std::runtime_error("restored session ID does not match selected session");
        } else {
            state.meta.session_id = session_id;
            state.meta.created_at = timestamp();
            state.meta.updated_at = state.meta.created_at;
            state.system_prompt.add_section("persona", "", config.system_prompt,
                model_io::SectionStability::Immutable);
        }
        state.tools = registry.get_tools();
        // Skills are host-owned sections. Rebuild only the prompt at startup:
        // remove obsolete skill sections and place current Growing skills before
        // Volatile sections without modifying any historical conversation record.
        model_io::PromptTemplate prompt;
        prompt.heading_level = state.system_prompt.heading_level;
        for (const auto& section : state.system_prompt) {
            if (!section.name.starts_with("skill.")
                && section.stability != model_io::SectionStability::Volatile)
                prompt.add_section(section.name, section.title, section.text, section.stability);
        }
        (void)registry.inject_skills(prompt);
        for (const auto& section : state.system_prompt) {
            if (!section.name.starts_with("skill.")
                && section.stability == model_io::SectionStability::Volatile)
                prompt.add_section(section.name, section.title, section.text, section.stability);
        }
        state.system_prompt = std::move(prompt);
    }

    /** Install the process-wide authoritative approval listener. */
    void install_confirmation() {
        auto& bus = eventbus::default_async_bus();
        if (bus.subscriber_count<tools::InvokeConfirmEvent>() != 0)
            throw std::runtime_error("worker requires one authoritative confirmation listener");
        confirmation = bus.subscribe<tools::InvokeConfirmEvent>(
            [weak = weak_from_this()](tools::InvokeConfirmEvent event)
                -> asio::awaitable<tools::InvokeConfirmEvent> {
                auto self = weak.lock();
                if (!self) {
                    event.decision = tools::ConfirmDecision::Denied;
                    event.reason = "worker no longer available";
                    co_return event;
                }
                std::shared_ptr<ConfirmationScope> current;
                std::string run;
                {
                    std::lock_guard lock(self->control_mutex);
                    current = self->scope;
                    run = self->run_id;
                }
                co_return co_await confirm(std::move(event), std::move(current), self->strand,
                    self->config.confirmation, self->config.confirmation_timeout,
                    self->session_id, std::move(run));
            });
    }

    /** Install persistence, event forwarding, and strand-routed controls. */
    void install_observers() {
        subscriptions.emplace_back(events.subscribe<loop::RunStarted>([this](const auto&) {
            emit("run_started");
        }));
        subscriptions.emplace_back(events.subscribe<loop::InputCommitted>([this](const auto&) {
            emit("input_committed");
        }));
        subscriptions.emplace_back(events.subscribe<loop::ModelCommitted>([this](const auto& event) {
            emit("model_response", event.state.turns.back().agent_loop_step.back().model_response);
        }));
        subscriptions.emplace_back(events.subscribe<loop::BeforeToolBatch>([this](const auto& event) {
            emit("tool_calls", event.calls);
        }));
        subscriptions.emplace_back(events.subscribe<loop::ToolDispatchCheckpoint>([this](const auto&) {
            save(SaveBoundary::BeforeTools);
        }));
        subscriptions.emplace_back(events.subscribe<loop::ToolResultsCheckpoint>([this](const auto&) {
            save(SaveBoundary::ResultsReady);
        }));
        subscriptions.emplace_back(events.subscribe<loop::ToolResultsCommitted>([this](const auto& event) {
            emit("tool_results", *event.state.turns.back().agent_loop_step.back().invoke_returns);
        }));
        // Metadata edits belong to writable transactions, never to the
        // read-only checkpoint observers. Recovery snapshots retain the last
        // logical edit timestamp rather than mutating state during publication.
        subscriptions.emplace_back(events.subscribe<loop::EditOnStepFinished>([](const auto& event) {
            event.state.meta.updated_at = timestamp();
        }));
        subscriptions.emplace_back(events.subscribe<loop::EditOnRunFinished>([](const auto& event) {
            event.state.meta.updated_at = timestamp();
        }));
        subscriptions.emplace_back(events.subscribe<loop::StepFinished>([this](const auto&) {
            if (config.save_step) save(SaveBoundary::StepFinished);
        }));
        subscriptions.emplace_back(events.subscribe<loop::RunFinished>([this](const auto&) {
            if (config.save_run) save(SaveBoundary::RunFinished);
        }));
        subscriptions.emplace_back(events.subscribe<io::SignalEvent>(
            [weak = weak_from_this()](const io::SignalEvent& event) {
            const auto& signal = event.signal;
            if (auto self = weak.lock()) {
                asio::post(self->strand, [self, signal] {
                    try {
                        const auto operation = signal.at("operation").template get<std::string>();
                        if (operation == "cancel") {
                            const auto run = signal.at("run_id").template get<std::string>();
                            if (run.empty()) throw std::invalid_argument("cancel requires run_id");
                            self->cancel(run);
                            self->emit("status", self->status());
                        } else if (operation == "shutdown") {
                            self->shutdown();
                        } else if (operation == "status") {
                            self->emit("status", self->status());
                        } else if (operation == "options") {
                            self->emit("options", self->options());
                        } else {
                            throw std::invalid_argument("unknown signal operation");
                        }
                    } catch (const std::exception& error) {
                        try { self->emit("error", {{"message", error.what()}}); }
                        catch (...) { self->fail(std::current_exception()); }
                    }
                });
            }
        }));
    }

    /** One writer drains owned event values without borrowing live state. */
    asio::awaitable<void> send_events() {
        while (outgoing.is_open() || outgoing.ready()) {
            Json message;
            try {
                message = co_await outgoing.async_receive(asio::use_awaitable);
            } catch (const boost::system::system_error& error) {
                if (error.code() == asio::experimental::channel_errc::channel_closed) co_return;
                throw;
            }
            co_await client.send(std::move(message));
        }
    }

    /** Serialized payload admission and loop execution; never overlaps runs. */
    asio::awaitable<void> consume() {
        auto payloads = client.subscribe_payload();
        emit("ready", status());
        while (!stopping) {
            auto payload = co_await payloads.next();
            if (stopping || shutdown_requested.load()) break;
            std::optional<Input> input;
            try {
                input = parse_input(payload);
                if (requests.contains(input->request_id))
                    throw std::invalid_argument("duplicate request_id in the recent admission window");
                if (state.loop && (state.loop->phase == model_io::LoopPhase::Tools
                    || state.loop->phase == model_io::LoopPhase::Blocked))
                    throw std::invalid_argument("session requires operator recovery inspection");
                if (!input->has_message && state.turns.empty())
                    throw std::invalid_argument("no turn to continue");
                // Validate confirmation on a value copy before invoking the
                // provider. If either category fails, neither selection changes.
                // Enum-only assignment after provider success cannot throw.
                auto next_confirmation = confirmation_options;
                if (input->options.contains("confirmation")) {
                    next_confirmation.handle_options(input->options.at("confirmation"));
                }
                if (input->options.contains("model")) {
                    model->handle_options(input->options.at("model"));
                }
                confirmation_options = next_confirmation;
            } catch (const std::exception& error) {
                emit("input_rejected", {{"request_id", payload.is_object() ? payload.value("request_id", Json()) : Json()},
                    {"message", error.what()}});
                continue;
            }
            request_id = input->request_id;
            if (request_order.size() == 4096) {
                requests.erase(request_order.front());
                request_order.pop_front();
            }
            requests.insert(request_id);
            request_order.push_back(request_id);
            {
                std::lock_guard lock(control_mutex);
                // Pair admission with cancel()'s snapshot under the same lock.
                // A stop racing this block either prevents admission or obtains
                // the freshly installed source; it cannot cancel only an old run.
                if (shutdown_requested.load()) {
                    stopping = true;
                    break;
                }
                scope = std::make_shared<ConfirmationScope>(confirmation_options.mode());
                run_stop = std::stop_source();
                run_id = new_identity();
            }
            active = true;
            state.meta.updated_at = timestamp();
            run_saved = false;
            emit("input_admitted");
            loop::RunResult result;
            try {
                result = co_await loop::run(*model, registry, events, strand, state,
                    input->has_message, std::move(input->message),
                    {config.max_exchanges}, run_stop.get_token());
            } catch (const std::exception& error) {
                result.status = loop::RunStatus::Failed;
                result.error = error.what();
            }
            // An earlier RunFinished observer can throw and prevent our slot
            // from running. The returned state is still final: complete a
            // required save here before reporting durability or admitting input.
            if (!storage_failed && !run_saved
                && (config.save_run || result.status == loop::RunStatus::Cancelled)) {
                save(config.save_run ? SaveBoundary::RunFinished : SaveBoundary::Cancelled);
            }
            active = false;
            {
                std::lock_guard lock(control_mutex);
                scope.reset();
            }
            if (storage_failed) {
                // Preserve the last known recovery file; never overwrite failure evidence.
                stopping = true;
                emit("error", {{"message", "required snapshot failed; worker is stopping"},
                    {"durable", false}});
            } else {
                emit("run_finished", {{"status", run_status(result.status)},
                    {"error", result.error}, {"exchanges", result.completed_exchanges},
                    {"durable", run_saved}});
            }
        }
    }

    /** Supervise IO and output tasks, then drain resources on every exit path. */
    asio::awaitable<void> run_owned() {
        co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
        // Ownership spans initialization, all saves, and final cleanup. A local
        // guard releases it even when startup throws while Application survives.
        std::unique_ptr<fileio::SessionLock> ownership;
        if (config.persistence) {
            const auto directory = config.storage / session_id;
            std::filesystem::create_directories(directory);
            ownership = std::make_unique<fileio::SessionLock>(directory / "session.lock");
        }
        initialize();
        asio::co_spawn(strand, client.run(), [self = shared_from_this()](std::exception_ptr error) {
            if (error) {
                // A blocked outbound send can wake before client.run() reports
                // its failure, just like the payload consumer. Prefer the
                // transport cause over those secondary channel diagnostics.
                if (channel_shutdown(self->failure)) self->failure = error;
                self->fail(error);
            }
            self->stopping = true;
            self->cancel();
            self->client_done.try_send(boost::system::error_code{}, true);
        });
        asio::co_spawn(strand, send_events(), [self = shared_from_this()](std::exception_ptr error) {
            if (error && !self->stopping) self->fail(error);
            self->sender_done.try_send(boost::system::error_code{}, true);
        });
        try {
            co_await consume();
        } catch (const boost::system::system_error& error) {
            // IO closes the payload queue before its supervisor reports the
            // underlying protocol/signal failure. Join that supervisor below
            // instead of replacing its primary diagnostic with channel_closed.
            if (error.code() != asio::experimental::channel_errc::channel_closed
                && !stopping) {
                fail(std::current_exception());
            }
        } catch (...) {
            if (!stopping) fail(std::current_exception());
        }
        stopping = true;
        cancel();
        try {
            if (config.save_shutdown && !storage_failed) {
                state.meta.updated_at = timestamp();
                save(SaveBoundary::Shutdown);
            }
        } catch (...) {
            if (!failure) failure = std::current_exception();
        }
        try {
            co_await store->shutdown();
        } catch (...) {
            if (!failure) failure = std::current_exception();
        }
        outgoing.close();
        // Final queue admission is bounded; admission is never a delivery receipt.
        auto deadline = std::make_shared<asio::steady_timer>(strand, std::chrono::milliseconds(500));
        deadline->async_wait([self = shared_from_this(), deadline](boost::system::error_code error) {
            if (!error) self->client.stop();
        });
        co_await sender_done.async_receive(asio::use_awaitable);
        deadline->cancel();
        client.stop();
        co_await client_done.async_receive(asio::use_awaitable);
        confirmation.disconnect();
        subscriptions.clear();
        if (failure) std::rethrow_exception(failure);
    }
};

Application::Application(asio::any_io_executor executor, load::Configuration config,
                         std::string session_id, std::shared_ptr<llm::LLMModel> model)
    : impl_(std::make_shared<Impl>(executor, std::move(config), std::move(session_id), std::move(model))) {}
Application::~Application() = default;
void Application::stop() { impl_->shutdown(); }
asio::awaitable<void> Application::run(std::stop_token stop) {
    auto self = impl_;
    if (self->started.exchange(true)) throw std::logic_error("Application::run is single-use");
    std::stop_callback on_stop(stop, [self] { self->shutdown(); });
    co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
    co_await asio::co_spawn(self->strand, self->run_owned(), asio::use_awaitable);
}
} // namespace core
