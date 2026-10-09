#include "core/application.hpp"
#include "versioning/version.hpp"
#include "fileio/session_lock.hpp"
#include "core/confirmation.hpp"
#include "core/protocol.hpp"
#include "core/compact.hpp"
#include "logging/logger.hpp"
#include "core/event_outbox.hpp"
#include "load/plugins.hpp"
#include "load/persistence.hpp"
#include "loop/loop.hpp"
#include "loop/events.hpp"
#include "loop/hook_registry.hpp"
#include "loop/intrinsic/context_statistic/hook.hpp"
#include "tools/intrinsic/process/toolset.hpp"
#include "tools/intrinsic/reading/toolset.hpp"
#include "tools/intrinsic/editing/toolset.hpp"
#include "tools/intrinsic/modality_assist/toolset.hpp"
#include "tools/intrinsic/hub_remote_call/toolset.hpp"
#include "tools/registry.hpp"
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <unordered_set>
#include <deque>
#include <iostream>
#include <ctime>
#include <type_traits>
#include <algorithm>
#include <charconv>
#include <limits>

namespace core {
namespace asio = boost::asio;
using Json = nlohmann::json;

namespace {
/**
 * Attach host execution identity inside the loop's integration transaction.
 * Input provenance stays untouched when Continue appends to an existing turn.
 * The provider still owns integration policy; only the newly appended step's
 * extras are annotated, before observers, recovery checkpoints or tools run.
 * This borrowed adapter lives until run() has joined its model coroutine.
 */
class ExecutionModel final : public llm::LLMModel {
public:
    ExecutionModel(asio::any_io_executor executor, llm::LLMModel& provider,
                   Json execution)
        : LLMModel(std::move(executor), Json::object()), provider_(provider),
          execution_(std::move(execution)) {}

    llm::LLMModelType model_type() const noexcept override {
        return provider_.model_type();
    }

    asio::awaitable<model_io::MessageItem> converse(
        model_io::AgentInputState conversation) override {
        co_return co_await provider_.converse(std::move(conversation));
    }

    void integrate(model_io::AgentInputState& state,
                   const model_io::MessageItem& item) override {
        provider_.integrate(state, item);
        if (item.type != model_io::MessageItemType::ModelResponse) return;
        if (state.turns.empty() || state.turns.back().agent_loop_step.empty()) {
            throw std::logic_error("model did not append a response step");
        }
        auto& extras = state.turns.back().agent_loop_step.back().extras;
        if (!extras || !extras->is_object()) extras = Json::object();
        (*extras)["simplex.execution"] = execution_;
    }

private:
    llm::LLMModel& provider_;
    Json execution_;
};

/**
 * Append configured environment hints after tool skills and before user Volatile
 * sections. These statements describe the environment; they do not change the
 * working directory, restrict access, or verify installed software. Empty
 * settings contribute no section. The caller removes any old host-owned copy.
 */
void inject_environment(
    model_io::PromptTemplate& prompt,
    const load::RuntimeEnvironment& environment
) {
    std::string text;
    if (!environment.workspace.empty()) {
        text = "Workspace: " + environment.workspace.string()
            + "\nThis is a working location hint, not an access restriction.";
    }
    if (!environment.platform.empty()) {
        if (!text.empty()) text += "\n\n";
        text += "Platform (configured): " + environment.platform;
    }
    std::string software;
    for (const auto& entry : environment.software) {
        if (!entry.empty()) software += "\n- " + entry;
    }
    if (!software.empty()) {
        if (!text.empty()) text += "\n\n";
        text += "Software (configured; availability not verified):" + software;
    }
    if (!text.empty()) {
        prompt.add_section(
            "environment.runtime",
            "Runtime Environment",
            text,
            model_io::SectionStability::Volatile
        );
    }
}

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

/**
 * Plan a never-reused archive pathname without changing disk contents.
 * Derive the ordinal from existing entries so restarts and wall-clock changes
 * cannot reorder archives. Failed attempts also consume their ordinal. The
 * session's existing ownership lock serializes cooperating worker processes.
 */
std::filesystem::path plan_archive(
    const std::filesystem::path& directory,
    const std::string& run
) {
    std::uint64_t latest = 0;
    if (std::filesystem::exists(directory)) {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            const auto name = entry.path().filename().string();
            if (name.size() < 21 || name[20] != '-') continue;
            std::uint64_t ordinal = 0;
            const auto parsed = std::from_chars(name.data(), name.data() + 20, ordinal);
            if (parsed.ec == std::errc{} && parsed.ptr == name.data() + 20) {
                latest = std::max(latest, ordinal);
            }
        }
    }
    if (latest == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("compact archive sequence exhausted");
    }
    auto ordinal = std::to_string(latest + 1);
    ordinal.insert(0, 20 - ordinal.size(), '0');
    auto time = timestamp();
    std::erase(time, ':');
    return directory / (ordinal + "-" + time + "-" + run);
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
        case loop::RunStatus::AutoCompactRequired: return "auto_compact_required";
        case loop::RunStatus::Failed: return "failed";
    }
    throw std::invalid_argument("invalid run status");
}

const char* failure_stage(loop::RunFailureStage stage) {
    switch (stage) {
        case loop::RunFailureStage::Other: return "other";
        case loop::RunFailureStage::ModelRequest: return "model_request";
    }
    throw std::invalid_argument("invalid run failure stage");
}
}


/** Strand-owned runtime, with thread-safe run control and bounded event admission. */
struct Application::Impl : std::enable_shared_from_this<Impl> {
    using ControlQueue = asio::experimental::concurrent_channel<
        void(boost::system::error_code, Json)>;
    using Done = asio::experimental::channel<void(boost::system::error_code, bool)>;

    Impl(asio::any_io_executor executor, load::Configuration configuration,
         std::string session, std::shared_ptr<llm::LLMModel> injected)
        : strand(asio::make_strand(executor)), config(std::move(configuration)),
          session_id(std::move(session)), driver_model(std::move(injected)),
          hooks(events), store(std::make_shared<tools::intrinsic::ProcessSessionStore>(strand)),
          client(strand, config.client, events, config.queues, config.transport),
          outgoing(config.event_capacity), outgoing_ready(strand, 1),
          control_messages(strand, config.queues.signal_capacity),
          control_done(strand, 1), sender_done(strand, 1), client_done(strand, 1) {
        validate_session_id(session_id);
        if (config.max_auto_compactions == 0 || config.max_exchanges == 0) {
            throw std::invalid_argument("worker execution budgets must be positive");
        }
        if (config.auto_compact_threshold && (!config.persistence || config.memory.empty()
            || config.auto_compact_prompt.empty() || config.auto_compact_continue_prompt.empty())) {
            throw std::invalid_argument("automatic compaction requires persistence, memory and operation prompts");
        }
        if (config.transport.write_byte_capacity < intercom::default_write_byte_capacity) {
            throw std::invalid_argument("transport byte capacity must cover the application budget");
        }
    }

    asio::strand<asio::any_io_executor> strand;
    load::Configuration config;
    std::string session_id;
    std::string worker_id = new_identity();
    std::shared_ptr<llm::LLMModel> driver_model;
    /** Optional model shared with the modality-assist toolset for isolated exchanges. */
    std::shared_ptr<llm::LLMModel> modality_assist_model;
    eventbus::EventBus events;
    tools::ToolRegistry registry;
    loop::LoopHookRegistry hooks;
    std::shared_ptr<tools::intrinsic::ProcessSessionStore> store;
    io::Client client;
    EventOutbox outgoing;
    Done outgoing_ready;
    /** Bounded bridge for status/options signals; cancellation acts before it. */
    ControlQueue control_messages;
    Done control_done;
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
    // All counters belong to one admitted request, not an individual loop call.
    std::size_t task_exchanges = 0;
    std::size_t compact_exchanges = 0;
    std::size_t compact_attempts = 0;
    std::size_t compact_successes = 0;
    std::string failure_operation = "task";
    enum class CompactMode { Manual, Automatic };
    std::exception_ptr failure;
    std::uint64_t history_revision = 0;

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
        if (shutdown_requested.exchange(true)) return;
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
        outgoing_ready.try_send(boost::system::error_code{}, true);
        control_messages.close();
    }

    /** Noncritical congestion is bounded and observable, never a run failure. */
    void emit(std::string name, Json data,
              const std::string& event_request, const std::string& event_run) {
        if (name == "run_finished" || (name == "error" && storage_failed)) {
            data["rejected_queries"] = client.rejected_queries();
            data["unreported_rejections"] = client.unreported_rejections();
        }
        auto kind = EventOutbox::Kind::Feedback;
        if (name == "model_response" || name == "tool_calls" || name == "tool_results") {
            kind = EventOutbox::Kind::Preview;
        } else if (name == "history" || name == "answer" || name == "history_error" || name == "answer_error") {
            kind = EventOutbox::Kind::Query;
        } else if (name == "persisted" || name == "status" || name == "options" || name == "export_error"
                   || (name == "error" && !storage_failed)
                   || (name == "compact_finished" && data.value("origin", std::string()) == "automatic")) {
            kind = EventOutbox::Kind::Metadata;
        } else if (name == "ready" || name == "input_admitted" || name == "run_started"
                   || name == "input_committed" || name == "run_finished" || name == "compact_finished"
                   || (name == "error" && storage_failed)) {
            kind = EventOutbox::Kind::Lifecycle;
        }
        Json message = {{"type", "event"}, {"event", std::move(name)},
            {"session_id", session_id}, {"worker_id", worker_id},
            {"request_id", event_request}, {"run_id", event_run}, {"data", std::move(data)}};
        const auto admission = outgoing.admit(std::move(message), kind, client.queued_write_bytes());
        if (admission == EventOutbox::Admission::Omitted && kind == EventOutbox::Kind::Lifecycle) {
            throw std::runtime_error("application lifecycle event admission failed");
        }
        if (admission != EventOutbox::Admission::Omitted) {
            outgoing_ready.try_send(boost::system::error_code{}, true);
        }
    }

    /** Ordinary events belong to the current run; rejected inputs never do. */
    void emit(std::string name, Json data = Json::object()) {
        if (name == "compact_finished") {
            data = display_compact(std::move(data));
        } else if (name != "model_response" && name != "tool_calls" && name != "tool_results"
                   && name != "history" && name != "answer") {
            data = display_value(data);
        }
        emit(std::move(name), std::move(data), request_id, run_id);
    }

    /** Report a rejected request without borrowing the active run's identity. */
    void reject_input(Json rejection) {
        const auto& id = rejection.at("request_id");
        const auto event_request = id.is_string() ? id.get<std::string>() : std::string();
        emit("input_rejected", std::move(rejection), event_request, "");
    }

    Json status() const {
        Json value = {{"active", active}, {"stopping", stopping},
            {"storage_failed", storage_failed}, {"rejected_payloads", client.rejected_payloads()},
            {"rejected_queries", client.rejected_queries()},
            {"unreported_rejections", client.unreported_rejections()},
            {"event_queue", {{"count", outgoing.size()}, {"bytes", outgoing.bytes()},
                             {"transport_bytes", client.queued_write_bytes()}}},
            {"capabilities", Json::array({"session-history", "context-compact", "auto-compact", "answer-pages"})}};
        value["memory_retention"] = {{"max_archives", config.memory_retention.max_archives}};
        value["auto_compact"] = compact_counters();
        if (state.loop) value["loop"] = *state.loop;
        return value;
    }

    /**
     * Read-only capability and current-selection snapshot. Empty reserved
     * categories do not imply that tools are disabled.
     */
    Json options() const {
        const llm::LLMModel& provider = *driver_model;
        return {
            {"model", {
                {"available", provider.get_options()},
                {"current", provider.get_current_options()}
            }},
            {"tools", {{"available", Json::array()}, {"current", Json::object()}}},
            {"confirmation", {
                {"available", confirmation_options.get_options()},
                {"current", confirmation_options.get_current_options()}
            }}
        };
    }

    /** Required JSON saves latch failure even if RunFinished swallows observers. */
    void save(SaveBoundary boundary) {
        if (!config.persistence || storage_failed) return;
        const auto directory = config.state_directory;
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
        if (!driver_model) {
            driver_model = plugins.providers.create_model(config.provider, strand, config.model);
        }
        if (!driver_model) {
            throw std::runtime_error("driver provider could not construct a model");
        }
        if (config.modality_assist_model) {
            const auto& selected = *config.modality_assist_model;
            modality_assist_model = plugins.providers.create_model(
                selected.provider, strand, selected.model);
            if (!modality_assist_model) {
                throw std::runtime_error("modality_assist_model provider could not construct a model");
            }
        }
        registry.add(std::make_shared<tools::intrinsic::ProcessToolSet>(
            store, &eventbus::default_async_bus()));
        registry.add(std::make_shared<tools::intrinsic::ReadingToolSet>());
        registry.add(std::make_shared<tools::intrinsic::EditingToolSet>());
        if (modality_assist_model) {
            registry.add(std::make_shared<tools::intrinsic::ModalityAssistToolSet>(
                modality_assist_model));
        }
        if (config.hub_remote_call) {
            registry.add(std::make_shared<tools::intrinsic::HubRemoteCallToolSet>(
                *config.hub_remote_call, config.hub_remote_call_timeout,
                [weak = weak_from_this()] {
                    auto self = weak.lock();
                    if (!self) throw std::runtime_error("worker no longer available");
                    std::lock_guard lock(self->control_mutex);
                    if (!self->scope) throw std::runtime_error("no active run");
                    return tools::intrinsic::HubRemoteCallIdentity{
                        self->worker_id, self->session_id, self->run_id};
                }));
        }
        for (auto& tool : extensions.tools) registry.add(std::move(tool));
        hooks.add(loop::intrinsic::ContextStatisticHook::from_config());
        for (auto& hook : extensions.loop_hooks) hooks.add(std::move(hook));
    }

    /** Restore history unchanged, reconciling only host-owned capabilities. */
    void restore_state() {
        const auto snapshot = config.state_directory / "state.json";
        const bool restored = config.persistence && config.restore && std::filesystem::exists(snapshot);
        if (restored) {
            state = load::load_state(snapshot);
            if (state.meta.session_id != session_id)
                throw std::runtime_error("restored session ID does not match selected session");
        } else {
            state.meta.session_id = session_id;
            state.meta.created_at = timestamp();
            state.meta.updated_at = state.meta.created_at;
            state.system_prompt = std::move(config.system_prompt);
        }
        state.tools = registry.get_tools();
        // Skills, environment.runtime, signature.runtime, and memory.runtime are host-owned.
        // replace old host sections and place skills and runtime hints before
        // Volatile sections without modifying any historical conversation record.
        model_io::PromptTemplate prompt;
        prompt.heading_level = state.system_prompt.heading_level;
        for (const auto& section : state.system_prompt) {
            if (!section.name.starts_with("skill.")
                && section.name != "environment.runtime"
                && section.name != "signature.runtime"
                && section.name != "memory.runtime"
                && section.stability != model_io::SectionStability::Volatile)
                prompt.add_section(section.name, section.title, section.text, section.stability);
        }
        (void)registry.inject_skills(prompt);
        inject_environment(prompt, config.environment);
        for (const auto& section : state.system_prompt) {
            if (!section.name.starts_with("skill.")
                && section.name != "environment.runtime"
                && section.name != "signature.runtime"
                && section.name != "memory.runtime"
                && section.stability == model_io::SectionStability::Volatile)
                prompt.add_section(section.name, section.title, section.text, section.stability);
        }
        // A decorative footer refreshed on restore, immediately before memory.
        std::string signature = "Welcome to simplex ";
        signature += simplex::VERSION_STRING;
        signature += ". Hello, " + (config.provider.empty() ? std::string("provider") : config.provider);
        signature += "! May your tasks go smoothly.";
        prompt.add_section(
            "signature.runtime",
            "",
            signature,
            model_io::SectionStability::Volatile
        );
        if (const auto memory = state.system_prompt.find("memory.runtime");
            memory != state.system_prompt.end()) {
            prompt.add_section("memory.runtime", "Memory", memory->text,
                model_io::SectionStability::Volatile);
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
                    self->worker_id, self->session_id, std::move(run));
            });
    }

    /** Install persistence, event forwarding, and strand-routed controls. */
    void install_observers() {
        subscriptions.emplace_back(events.subscribe<loop::InputCommitted>([this](const auto&) {
            ++history_revision;
            const auto& message = state.turns.back().user_input;
            if (!message.extras || message.extras->value("simplex.internal_input", "")
                != "auto_compact_continue") {
                emit("input_committed");
            }
        }));
        subscriptions.emplace_back(events.subscribe<loop::ModelCommitted>([this](const auto& event) {
            ++history_revision;
            Json projected = project_response(event.state, event.state.turns.size() - 1,
                event.state.turns.back().agent_loop_step.size() - 1, worker_id);
            emit("model_response", std::move(projected));
        }));
        subscriptions.emplace_back(events.subscribe<loop::BeforeToolBatch>([this](const auto& event) {
            emit("tool_calls", display_calls(event.calls));
        }));
        subscriptions.emplace_back(events.subscribe<loop::ToolDispatchCheckpoint>([this](const auto&) {
            save(SaveBoundary::BeforeTools);
        }));
        subscriptions.emplace_back(events.subscribe<loop::ToolResultsCheckpoint>([this](const auto&) {
            save(SaveBoundary::ResultsReady);
        }));
        subscriptions.emplace_back(events.subscribe<loop::ToolResultsCommitted>([this](const auto& event) {
            emit("tool_results", display_results(*event.state.turns.back().agent_loop_step.back().invoke_returns));
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
            ++history_revision;
        }));
        subscriptions.emplace_back(events.subscribe<loop::RunFinished>([this](const auto&) {
            ++history_revision;
        }));
        subscriptions.emplace_back(events.subscribe<loop::StepFinished>([this](const auto&) {
            if (config.save_step) save(SaveBoundary::StepFinished);
        }));
        subscriptions.emplace_back(events.subscribe<loop::RunFinished>([this](const auto&) {
            if (config.save_run) save(SaveBoundary::RunFinished);
        }));
        subscriptions.emplace_back(events.subscribe<io::SignalEvent>(
            [weak = weak_from_this()](const io::SignalEvent& event) {
                if (auto self = weak.lock()) {
                    const auto& signal = event.signal;
                    // This callback runs on the separate control thread. Only
                    // thread-safe run control acts here; all state reads remain
                    // in the bounded strand mailbox below.
                    if (signal.is_object() && signal.value("operation", Json()) == "shutdown") {
                        self->shutdown();
                        return;
                    }
                    if (signal.is_object() && signal.value("operation", Json()) == "cancel"
                        && signal.contains("run_id") && signal.at("run_id").is_string()
                        && !signal.at("run_id").get_ref<const std::string&>().empty()) {
                        self->cancel(signal.at("run_id").get<std::string>());
                    }
                    self->control_messages.try_send(boost::system::error_code{}, signal);
                }
            }));
        subscriptions.emplace_back(events.subscribe<io::PayloadQueryEvent>([this](const auto& event) {
            // IO publishes queries on the executor supplied to Client: our strand.
            // Do not repeatedly project/hash/encode state only to discard the
            // reply. That work can delay model cancellation and run completion
            // under a polling flood, even though queue storage stays bounded.
            if (outgoing.omit_query_when_congested(client.queued_write_bytes())) {
                return;
            }
            const auto& payload = event.payload;
            const bool answer = payload.value("operation", Json()) == "answer";
            try {
                if (answer) {
                    emit("answer", answer_page(state, payload, worker_id));
                } else {
                    auto request = parse_history_request(payload);
                    emit("history", history_page(state, request, history_revision, worker_id));
                }
            } catch (const std::exception& error) {
                emit(answer ? "answer_error" : "history_error", {
                    {"request_id", payload.value("request_id", Json())}, {"message", error.what()}});
            }
        }));
        subscriptions.emplace_back(events.subscribe<io::PayloadQueryRejectedEvent>([this](const auto& event) {
            emit(event.operation == "answer" ? "answer_error" : "history_error", {
                {"request_id", event.request_id}, {"code", event.code},
                {"message", "Worker query quota exceeded; retry after the connection drains."}});
        }));
        subscriptions.emplace_back(events.subscribe<io::PayloadRejectedEvent>([this](const auto& event) {
            if (stopping || shutdown_requested.load()) return;
            Json rejection = {{"request_id", event.request_id}, {"code", "payload_queue_full"},
                {"message", "Worker input queue is full. Wait for current work to finish, then retry."}};
            if (event.operation == "message" || event.operation == "continue" || event.operation == "compact") {
                rejection["operation"] = event.operation;
            }
            reject_input(std::move(rejection));
        }));
    }

    /** Drain only metadata/control requests; cancellation never waits for this task. */
    asio::awaitable<void> process_control() {
        for (;;) {
            boost::system::error_code error;
            auto signal = co_await control_messages.async_receive(
                asio::redirect_error(asio::use_awaitable, error));
            if (error || stopping) co_return;
            try {
                const auto operation = signal.at("operation").get<std::string>();
                if (operation == "cancel") {
                    if (signal.at("run_id").get<std::string>().empty()) {
                        throw std::invalid_argument("cancel requires run_id");
                    }
                    emit("status", status());
                } else if (operation == "status") {
                    emit("status", status());
                } else if (operation == "options") {
                    emit("options", options());
                } else {
                    throw std::invalid_argument("unknown signal operation");
                }
            } catch (const std::exception& error) {
                emit("error", {{"message", error.what()}});
            }
        }
    }

    /** Retain the front reservation until IO admission settles, even while suspended. */
    asio::awaitable<void> send_events() {
        for (;;) {
            if (outgoing.empty()) {
                if (outgoing.closed()) co_return;
                co_await outgoing_ready.async_receive(asio::use_awaitable);
                continue;
            }
            auto message = outgoing.begin_send();
            try {
                co_await client.send(std::move(message));
            } catch (...) {
                outgoing.complete_send();
                throw;
            }
            outgoing.complete_send();
        }
    }

    /** Retention never removes the current attempt or absolute archive
     * references in the authoritative state, including legacy memory prompts.
     */
    load::ArchiveCleanup cleanup_archive(const std::filesystem::path& root,
                                        const std::filesystem::path& current) {
        const auto references = compact_archive_references(state, root);
        return load::prune_memory_archives(root, current, config.memory_retention, references);
    }

    /** Run retention on failure/cancellation without replacing its diagnostic.
     * Export errors remain optional: report through bounded events and logging,
     * even if the transport cannot admit the notification. No exception escapes.
     */
    void cleanup_failed_compact(const std::filesystem::path& root,
                               const std::filesystem::path& current) noexcept {
        try {
            const auto cleaned = cleanup_archive(root, current);
            logging::Logger::info("compact archive cleanup: removed {} archives ({} bytes)",
                cleaned.removed_archives, cleaned.removed_bytes);
        } catch (const std::exception& error) {
            try {
                logging::Logger::warning("compact archive cleanup failed: {}", error.what());
                emit("export_error", {{"operation", "archive_cleanup"}, {"message", error.what()}});
            } catch (...) {
                // Optional diagnostics must not replace the original failure.
            }
        } catch (...) {
            // Preserve an original non-standard failure during unwinding too.
        }
    }

    /**
     * Summarize a private conversation copy, then publish a durable replacement.
     * The ordinary hook bus and automatic saves never observe the temporary
     * compact turn. History queries continue to see the original state while
     * the model is suspended. A fresh local bus prohibits tool dispatch even
     * when a provider ignores the omitted tool definitions and prompt.
     *
     * Archive directories are exclusively created and never reused. Successful
     * and failed attempts remain in persistent sequence order for inspection.
     * Every settled archived attempt applies retention, protecting the current
     * evidence and references in retained state. Preflight creates no archive.
     * The JSON save is the commit boundary: before it succeeds, the live state
     * is untouched. A published-but-unsynced write stops the worker just like
     * other required snapshot failures. Cancellation during the synchronous
     * commit does not roll back an already published snapshot.
     */
    asio::awaitable<loop::RunResult> compact(
        CompactMode mode, const std::string& call_id = {}) {
        const auto stop = run_stop.get_token();
        if (stop.stop_requested()) {
            loop::RunResult cancelled;
            cancelled.status = loop::RunStatus::Cancelled;
            co_return cancelled;
        }
        const auto memory_directory = std::filesystem::absolute(
            config.memory).lexically_normal();
        const auto archive_directory = plan_archive(memory_directory, run_id);
        const auto archive_file = archive_directory / "state.md";
        auto plan = plan_compact(state, memory_directory, archive_file);
        std::filesystem::create_directories(memory_directory);
        if (!std::filesystem::create_directory(archive_directory)) {
            throw std::runtime_error("compact archive directory already exists");
        }
        bool cleanup_attempted = false;
        // Run after the private draft and hook subscriptions are destroyed.
        // Synchronous best-effort cleanup cannot alter the primary outcome.
        struct CleanupAttempt {
            Impl& self;
            const std::filesystem::path& root;
            const std::filesystem::path& current;
            bool& attempted;
            ~CleanupAttempt() {
                if (!attempted) self.cleanup_failed_compact(root, current);
            }
        } cleanup{*this, memory_directory, archive_directory, cleanup_attempted};
        load::save_state(archive_file, state, load::StateFormat::Readable);

        auto draft = state;
        draft.tools.clear();
        eventbus::EventBus compact_events;
        // This built-in hook is stateless. Account the summary response before
        // pruning so the next ordinary run sees no commit-sequence gap.
        auto statistics = loop::LoopHookInterface::attach(
            hooks.get(loop::intrinsic::ContextStatisticHook::kName), compact_events);
        tools::ToolRegistry no_tools;
        auto prohibit_tools = compact_events.subscribe<loop::BeforeToolBatch>(
            [](const auto&) {
                throw std::runtime_error("compact response must not call tools");
            });
        model_io::MessageItem instruction;
        instruction.type = model_io::MessageItemType::UserInput;
        instruction.role = "user";
        model_io::Content content;
        content.raw = mode == CompactMode::Manual
            ? config.compact_prompt : config.auto_compact_prompt;
        content.raw += "\n\nOutput size constraint: the complete summary must be at most "
            + std::to_string(plan.summary_bytes)
            + " UTF-8 bytes (not tokens or JSON-escaped bytes). Keep the essential "
              "goals, state and retrieval information within that allowance. "
              "The replacement context budget is "
            + std::to_string(compact_context_max_bytes) + " bytes; fixed overhead is "
            + std::to_string(plan.fixed_bytes) + " bytes.";
        instruction.content.push_back(std::move(content));
        auto result = co_await loop::run(
            *driver_model, no_tools, compact_events, strand, draft, true,
            std::move(instruction), {std::nullopt, 0}, stop);
        add_exchanges(compact_exchanges, result.completed_exchanges);
        if (result.status != loop::RunStatus::Completed) {
            if (result.status != loop::RunStatus::Cancelled
                && result.status != loop::RunStatus::Failed) {
                result.status = loop::RunStatus::Failed;
                result.error = "compact returned an unexpected loop boundary";
            }
            co_return result;
        }
        // A model may complete concurrently with cancellation. Until the
        // replacement save begins, cancellation still preserves the old state.
        if (stop.stop_requested()) {
            result.status = loop::RunStatus::Cancelled;
            co_return result;
        }
        if (draft.turns.empty() || draft.turns.back().agent_loop_step.empty()) {
            throw std::runtime_error("compact produced no response");
        }
        const auto& response = draft.turns.back().agent_loop_step.back().model_response;
        std::string summary;
        for (const auto& part : response.content) {
            if (part.type == model_io::ContentType::Text && !part.raw.empty()) {
                if (!summary.empty()) summary += "\n\n";
                summary += part.raw;
            }
        }
        if (summary.find_first_not_of(" \t\r\n") == std::string::npos) {
            throw std::runtime_error("compact produced an empty text summary");
        }
        // Copy only the retained fields; never copy the heavy history again.
        // Retain provider-specific extras; transfer only the built-in usage
        // checkpoint produced by the private run before pruning its response.
        model_io::AgentInputState replacement;
        replacement.meta = state.meta;
        replacement.meta.updated_at = timestamp();
        replacement.tools = state.tools;
        replacement.extras = state.extras;
        model_io::sync_external_status(replacement,
            loop::intrinsic::ContextStatisticHook::kName,
            model_io::external_status(draft, loop::intrinsic::ContextStatisticHook::kName).value());
        replacement.loop = std::move(draft.loop);
        replacement.system_prompt = compact_prompt(std::move(plan), summary);
        const auto before_bytes = compact_context_bytes(state);
        const auto after_bytes = compact_context_bytes(replacement);
        if (after_bytes > compact_context_max_bytes) {
            throw std::runtime_error("compact replacement exceeds byte budget: replacement_bytes="
                + std::to_string(after_bytes) + ", budget_bytes="
                + std::to_string(compact_context_max_bytes));
        }
        const auto minimum_savings = before_bytes / 10 + (before_bytes % 10 != 0);
        if (after_bytes >= before_bytes ||
            before_bytes - after_bytes < minimum_savings) {
            throw std::runtime_error("compact did not reduce context by at least 10%");
        }
        // Refresh prompt-size estimates after memory injection; the reconciled
        // checkpoint prevents this second update from counting the cost twice.
        compact_events.publish(loop::EditOnRunFinished{replacement, result});
        const auto removed_turns = state.turns.size();
        // Allocate the success event before committing so construction failures
        // cannot be mistaken for an uncommitted operation.
        Json completed = {{"summary", summary}, {"memory_file", archive_file.string()},
            {"removed_turns", removed_turns}, {"revision", history_revision + 1},
            {"durable", true}};
        if (mode == CompactMode::Automatic) {
            completed["origin"] = "automatic";
            completed["cycle"] = compact_attempts;
            completed["call_id"] = call_id;
        }
        if (stop.stop_requested()) {
            result.status = loop::RunStatus::Cancelled;
            co_return result;
        }
        try {
            load::save_state(config.state_directory / "state.json", replacement);
        } catch (...) {
            storage_failed = true;
            if (!failure) failure = std::current_exception();
            cancel();
            throw;
        }
        static_assert(std::is_nothrow_move_assignable_v<model_io::AgentInputState>);
        state = std::move(replacement);
        ++history_revision;
        run_saved = true;
        emit("persisted", {{"boundary", "compact"}, {"format", "json"}});
        if (config.readable) {
            try {
                load::save_state(config.state_directory / "readable.md",
                    state, load::StateFormat::Readable);
            } catch (const std::exception& error) {
                emit("export_error", {{"message", error.what()}});
            }
        }
        // Retention sees the committed replacement. The exit guard handles
        // failed/cancelled attempts against the unchanged authoritative state.
        cleanup_attempted = true;
        try {
            const auto cleaned = cleanup_archive(memory_directory, archive_directory);
            completed["archive_cleanup"] = {{"removed_archives", cleaned.removed_archives},
                {"removed_bytes", cleaned.removed_bytes}};
        } catch (const std::exception& error) {
            completed["archive_cleanup_error"] = error.what();
        }
        emit("compact_finished", std::move(completed));
        co_return result;
    }

    /** Checked counters cannot silently wrap the final wire exchange total. */
    static void add_exchanges(std::size_t& total, std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() - total) {
            throw std::overflow_error("logical run exchange counter exhausted");
        }
        total += count;
    }

    Json compact_counters() const {
        return {{"attempts", compact_attempts}, {"succeeded", compact_successes},
            {"limit", config.max_auto_compactions},
            {"threshold", config.auto_compact_threshold}};
    }

    /** Only settled host memory allows continuing an otherwise empty session. */
    bool can_continue_memory() const {
        if (!state.turns.empty() || !state.loop
            || state.loop->phase != model_io::LoopPhase::Ready
            || !state.loop->pending_results.empty()
            || config.auto_compact_continue_prompt.empty()) return false;
        for (const auto& section : state.system_prompt) {
            if (section.name == "memory.runtime"
                && section.text.find_first_not_of(" \t\r\n") != std::string::npos) return true;
        }
        return false;
    }

    /** Host-only provenance is stored on the message, never on user content. */
    model_io::MessageItem continuation() const {
        model_io::MessageItem message;
        message.type = model_io::MessageItemType::UserInput;
        message.role = "user";
        model_io::Content content;
        content.raw = config.auto_compact_continue_prompt;
        message.content.push_back(std::move(content));
        message.extras = Json{{"simplex.internal_input", "auto_compact_continue"},
            {"simplex.source", {{"worker_id", worker_id},
                {"request_id", request_id}, {"run_id", run_id}}}};
        return message;
    }

    /** Record a host failure/cancellation without replaying loop finish hooks. */
    void finish_controller(const loop::RunResult& result) {
        if (state.loop && state.loop->phase == model_io::LoopPhase::Ready) {
            state.loop->status = result.status == loop::RunStatus::Cancelled
                ? model_io::LoopStatus::Cancelled : model_io::LoopStatus::Failed;
            state.loop->error = result.error;
            ++history_revision;
            run_saved = false;
        }
    }

    /**
     * One logical request owns every segment, summary and private continuation.
     * Cancellation is never reset. Guards run only after tools have settled;
     * a rejected cycle creates neither an archive nor a synthetic pending call.
     */
    asio::awaitable<loop::RunResult> run_with_auto_compact(
        bool has_message, model_io::MessageItem message) {
        if (!has_message && can_continue_memory()) {
            has_message = true;
            message = continuation();
        }
        ExecutionModel execution_model(strand, *driver_model,
            {{"worker_id", worker_id}, {"request_id", request_id}, {"run_id", run_id}});
        for (;;) {
            failure_operation = "task";
            run_saved = false;
            auto result = co_await loop::run(execution_model, registry, events, strand,
                state, has_message, std::move(message),
                {config.max_exchanges, config.auto_compact_threshold}, run_stop.get_token());
            add_exchanges(task_exchanges, result.completed_exchanges);
            if (result.status != loop::RunStatus::AutoCompactRequired) co_return result;
            if (storage_failed) {
                result.status = loop::RunStatus::Failed;
                result.error = "required snapshot failed; worker is stopping";
                finish_controller(result);
                co_return result;
            }
            if (run_stop.stop_requested()) {
                result.status = loop::RunStatus::Cancelled;
                finish_controller(result);
                co_return result;
            }
            Json trigger = {{"reason", result.auto_compact_reason == loop::AutoCompactReason::TokenThreshold
                    ? "token_threshold" : "exchange_limit"},
                {"threshold", config.auto_compact_threshold}, {"max_exchanges", config.max_exchanges},
                {"segment_exchanges", result.completed_exchanges},
                {"last_exchange_tokens", result.last_exchange_tokens
                    ? Json(*result.last_exchange_tokens) : Json(nullptr)},
                {"succeeded", compact_successes}, {"limit", config.max_auto_compactions}};
            failure_operation = "auto_compact";
            if ((compact_successes && result.completed_exchanges == 1)
                || compact_attempts >= config.max_auto_compactions) {
                result.status = loop::RunStatus::Failed;
                result.error = compact_successes && result.completed_exchanges == 1
                    ? "auto_compact_too_frequent: requested again after one exchange; review limits/context size: "
                    : "auto_compact_limit: per-request attempt budget exhausted; review limits/context size: ";
                result.error += trigger.dump();
                finish_controller(result);
                co_return result;
            }
            ++compact_attempts;
            trigger["cycle"] = compact_attempts;
            model_io::InvokeQuery query;
            query.id = "host-auto-compact-" + new_identity();
            query.name = "auto_compact";
            query.type = model_io::InvokeType::SerialWrite;
            query.security = model_io::InvokeSecurity::Trusted;
            query.arguments = std::move(trigger);
            query.extras = Json{{"origin", "worker"}, {"operation", "auto_compact"}};
            emit("tool_calls", display_calls({query}));
            try {
                result = co_await compact(CompactMode::Automatic, query.id);
            } catch (const std::exception& error) {
                result.status = loop::RunStatus::Failed;
                result.failure_stage = loop::RunFailureStage::Other;
                result.error = error.what();
            }
            if (result.status == loop::RunStatus::Completed) ++compact_successes;
            model_io::InvokeReturn record;
            record.query = query;
            record.output.raw = result.status == loop::RunStatus::Completed
                ? "Context compacted."
                : "Automatic compaction " + run_status(result.status) + ": " + result.error;
            record.extras = Json{{"status", run_status(result.status)}};
            if (result.status == loop::RunStatus::Failed) {
                (*record.extras)["error"] = {{"stage", "auto_compact"}, {"message", result.error}};
            }
            model_io::MessageItem returned;
            returned.type = model_io::MessageItemType::InvokeReturn;
            returned.role = "tool";
            returned.content.push_back(record.output);
            returned.invoke_return = std::move(record);
            emit("tool_results", display_results({returned}));
            if (result.status != loop::RunStatus::Completed) {
                finish_controller(result);
                co_return result;
            }
            if (run_stop.stop_requested()) {
                result.status = loop::RunStatus::Cancelled;
                finish_controller(result);
                co_return result;
            }
            has_message = true;
            message = continuation();
        }
    }

    /** Drain a finite backlog before admitting more lifecycle production.
     * Queries/status can still be processed, but their noncritical output is
     * temporarily omitted. They cannot prolong this wait by adding new entries.
     * Cancellation/shutdown remain independent, and every exit resumes admission.
     */
    asio::awaitable<void> wait_for_admission() {
        outgoing.pause_noncritical(true);
        struct ResumeOutput {
            EventOutbox& outbox;
            ~ResumeOutput() { outbox.pause_noncritical(false); }
        } resume{outgoing};
        while (!stopping && !shutdown_requested.load()
               && (!outgoing.empty() || client.queued_write_bytes() != 0)) {
            asio::steady_timer wait(strand, std::chrono::milliseconds(10));
            co_await wait.async_wait(asio::use_awaitable);
        }
    }

    /** Serialized payload admission and loop execution; never overlaps runs. */
    asio::awaitable<void> consume() {
        auto payloads = client.subscribe_payload();
        emit("ready", status());
        while (!stopping) {
            auto payload = co_await payloads.next();
            co_await wait_for_admission();
            if (stopping || shutdown_requested.load()) break;
            std::optional<Input> input;
            bool applying_options = false;
            try {
                input = parse_input(payload);
                if (requests.contains(input->request_id))
                    throw std::invalid_argument("duplicate request_id in the recent admission window");
                if (state.loop && (state.loop->phase == model_io::LoopPhase::Tools
                    || state.loop->phase == model_io::LoopPhase::Blocked))
                    throw std::invalid_argument("session requires operator recovery inspection");
                if (input->operation == InputOperation::Compact) {
                    if (!config.persistence) {
                        throw std::invalid_argument("compact requires persistence.enabled");
                    }
                    if (state.loop && state.loop->phase != model_io::LoopPhase::Ready) {
                        throw std::invalid_argument("compact requires a settled ready state");
                    }
                    if (config.memory.empty() || config.compact_prompt.empty()) {
                        throw std::invalid_argument("compact configuration is incomplete");
                    }
                }
                if (!input->has_message && state.turns.empty()
                    && !(input->operation == InputOperation::Continue && can_continue_memory())) {
                    throw std::invalid_argument(input->operation == InputOperation::Compact
                        ? "no turns to compact" : "no turn to continue");
                }
                // Validate confirmation on a value copy before invoking the
                // provider. If either category fails, neither selection changes.
                // Enum-only assignment after provider success cannot throw.
                applying_options = true;
                auto next_confirmation = confirmation_options;
                if (input->options.contains("confirmation")) {
                    next_confirmation.handle_options(input->options.at("confirmation"));
                }
                if (input->options.contains("model")) {
                    driver_model->handle_options(input->options.at("model"));
                }
                confirmation_options = next_confirmation;
            } catch (const std::exception& error) {
                Json rejection = {
                    {"request_id", payload.is_object() ? payload.value("request_id", Json()) : Json()},
                    {"message", error.what()}
                };
                // Keep the requested operation in the replayable event. Hub
                // request records can expire before the transcript does.
                if (payload.is_object() && payload.contains("operation")
                    && payload.at("operation").is_string()) {
                    const auto operation = payload.at("operation").get<std::string>();
                    if (operation == "message" || operation == "continue"
                        || operation == "compact") {
                        rejection["operation"] = operation;
                    }
                }
                if (applying_options || dynamic_cast<const InputOptionsError*>(&error)) {
                    rejection["code"] = "invalid_options";
                }
                reject_input(std::move(rejection));
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
            if (input->operation != InputOperation::Compact) {
                state.meta.updated_at = timestamp();
            }
            run_saved = false;
            task_exchanges = compact_exchanges = compact_attempts = compact_successes = 0;
            failure_operation = input->operation == InputOperation::Compact ? "compact" : "task";
            emit("input_admitted", {{"operation", operation_name(input->operation)}});
            emit("run_started");
            loop::RunResult result;
            try {
                if (input->operation == InputOperation::Compact) {
                    result = co_await compact(CompactMode::Manual);
                } else {
                    result = co_await run_with_auto_compact(
                        input->has_message, std::move(input->message));
                }
            } catch (const std::exception& error) {
                result.status = loop::RunStatus::Failed;
                result.error = error.what();
                if (input->operation != InputOperation::Compact) finish_controller(result);
            }
            std::size_t total_exchanges = task_exchanges;
            add_exchanges(total_exchanges, compact_exchanges);
            result.completed_exchanges = total_exchanges;
            // An earlier RunFinished observer can throw and prevent our slot
            // from running. The returned state is still final: complete a
            // required save here before reporting durability or admitting input.
            if (input->operation != InputOperation::Compact && !storage_failed && !run_saved
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
                Json finished = {{"status", run_status(result.status)},
                    {"error", result.error}, {"exchanges", result.completed_exchanges},
                    {"durable", run_saved}, {"task_exchanges", task_exchanges},
                    {"compact_exchanges", compact_exchanges}, {"auto_compact", compact_counters()}};
                if (result.status == loop::RunStatus::Failed) {
                    finished["failure"] = {
                        {"stage", failure_stage(result.failure_stage)},
                        {"operation", failure_operation},
                        {"can_continue", can_continue_memory() || (!state.turns.empty() && state.loop
                            && state.loop->phase == model_io::LoopPhase::Ready)}
                    };
                }
                emit("run_finished", std::move(finished));
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
            const auto directory = config.storage;
            std::filesystem::create_directories(directory);
            ownership = std::make_unique<fileio::SessionLock>(directory / "session.lock");
        }
        initialize();
        asio::co_spawn(strand, process_control(),
            [self = shared_from_this()](std::exception_ptr error) {
                if (error && !self->stopping) self->fail(error);
                self->control_done.try_send(boost::system::error_code{}, true);
            });
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
        shutdown_requested.store(true);
        cancel();
        control_messages.close();
        co_await control_done.async_receive(asio::use_awaitable);
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
        outgoing_ready.try_send(boost::system::error_code{}, true);
        // Bound both application admission and transport drain. send_events()
        // acknowledges queue admission, so stopping immediately after joining
        // it can otherwise abort the last required-snapshot error in flight.
        // Completed local writes still do not acknowledge peer processing.
        auto deadline = std::make_shared<asio::steady_timer>(strand, std::chrono::milliseconds(500));
        deadline->async_wait([self = shared_from_this(), deadline](boost::system::error_code error) {
            if (!error) self->client.stop();
        });
        co_await sender_done.async_receive(asio::use_awaitable);
        while (client.queued_write_bytes() != 0
               && std::chrono::steady_clock::now() < deadline->expiry()) {
            asio::steady_timer drain(strand, std::chrono::milliseconds(1));
            co_await drain.async_wait(asio::use_awaitable);
        }
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
