#include "core/application.hpp"
#include "persistence/selection.hpp"
#include <boost/asio/thread_pool.hpp>
#include "versioning/version.hpp"
#include "fileio/session_lock.hpp"
#include "fileio/atomic_write.hpp"
#include "core/confirmation.hpp"
#include "core/protocol.hpp"
#include "core/compact.hpp"
#include "logging/logger.hpp"
#include "core/event_outbox.hpp"
#include "core/wire.hpp"
#include "load/plugins.hpp"
#include "persistence/store.hpp"
#include "loop/loop.hpp"
#include "loop/events.hpp"
#include "loop/hook_registry.hpp"
#include "loop/intrinsic/context_statistic/hook.hpp"
#include "tools/intrinsic/process/toolset.hpp"
#include "tools/intrinsic/reading/toolset.hpp"
#include "tools/intrinsic/archive_memory/toolset.hpp"
#include "tools/intrinsic/editing/toolset.hpp"
#include "tools/intrinsic/modality_assist/toolset.hpp"
#include "tools/intrinsic/hub_remote_call/toolset.hpp"
#include "tools/registry.hpp"
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <unordered_set>
#include <deque>
#include <iostream>
#include <type_traits>
#include <algorithm>
#include <limits>
#include <optional>

namespace core {
namespace asio = boost::asio;
using Json = nlohmann::json;

namespace {
/**
 * Attach host execution identity inside the loop's integration transaction.
 * New inputs receive their creating execution; Continue keeps that provenance
 * while annotating every new response with its own execution.
 * The provider still owns integration policy; only newly integrated input/step
 * metadata is annotated, before observers, recovery checkpoints or tools run.
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
        if (item.type == model_io::MessageItemType::UserInput) {
            if (state.turns.empty()) {
                throw std::logic_error("model did not append an input turn");
            }
            auto& extras = state.turns.back().user_input.extras;
            if (!extras || !extras->is_object()) extras = Json::object();
            (*extras)["simplex.source"] = execution_;
            return;
        }
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

// ---- Application-owned vocabularies ---------------------------------------
// The protocol package publishes these spellings; these converters keep the
// application from restating them at each emission site.

protocol::RunStatus wire_status(loop::RunStatus status) {
    switch (status) {
        case loop::RunStatus::Completed: return protocol::RunStatus::completed;
        case loop::RunStatus::Cancelled: return protocol::RunStatus::cancelled;
        case loop::RunStatus::ExchangeLimit: return protocol::RunStatus::exchange_limit;
        case loop::RunStatus::Failed: return protocol::RunStatus::failed;
        case loop::RunStatus::AutoCompactRequired:
            // auto_compact_required is an intermediate boundary: the host always
            // resolves it before a terminal outcome is published.
            throw std::logic_error("auto_compact_required is not a terminal outcome");
    }
    throw std::invalid_argument("invalid run status");
}

protocol::FailureStage wire_stage(loop::RunFailureStage stage) {
    switch (stage) {
        case loop::RunFailureStage::Other: return protocol::FailureStage::other;
        case loop::RunFailureStage::ModelRequest: return protocol::FailureStage::model_request;
    }
    throw std::invalid_argument("invalid run failure stage");
}

protocol::PersistBoundary wire_boundary(persistence::Boundary boundary) {
    switch (boundary) {
        case persistence::Boundary::BeforeTools: return protocol::PersistBoundary::before_tools;
        case persistence::Boundary::ResultsReady: return protocol::PersistBoundary::results_ready;
        case persistence::Boundary::StepFinished: return protocol::PersistBoundary::step_finished;
        case persistence::Boundary::RunFinished: return protocol::PersistBoundary::run_finished;
        case persistence::Boundary::Cancelled: return protocol::PersistBoundary::cancelled;
        case persistence::Boundary::Shutdown: return protocol::PersistBoundary::shutdown;
        case persistence::Boundary::Compaction: return protocol::PersistBoundary::compact;
    }
    throw std::invalid_argument("invalid persistence boundary");
}

protocol::StorageKind wire_backend(persistence::Backend backend) {
    switch (backend) {
        case persistence::Backend::Json: return protocol::StorageKind::json;
        case persistence::Backend::Sqlite: return protocol::StorageKind::sqlite;
    }
    throw std::invalid_argument("invalid storage backend");
}

protocol::InputOperation wire_operation(InputOperation operation) {
    switch (operation) {
        case InputOperation::Message: return protocol::InputOperation::message;
        case InputOperation::Continue: return protocol::InputOperation::continue_request;
        case InputOperation::Compact: return protocol::InputOperation::compact;
    }
    throw std::invalid_argument("invalid input operation");
}

/** The input operation with this spelling, or nothing for any other request. */
std::optional<protocol::InputOperation> input_operation(const std::string& name) {
    if (name == "message") return protocol::InputOperation::message;
    if (name == "continue") return protocol::InputOperation::continue_request;
    if (name == "compact") return protocol::InputOperation::compact;
    return std::nullopt;
}

protocol::LoopStatus wire_loop_status(model_io::LoopStatus status) {
    switch (status) {
        case model_io::LoopStatus::Idle: return protocol::LoopStatus::idle;
        case model_io::LoopStatus::Running: return protocol::LoopStatus::running;
        case model_io::LoopStatus::Completed: return protocol::LoopStatus::completed;
        case model_io::LoopStatus::Cancelled: return protocol::LoopStatus::cancelled;
        case model_io::LoopStatus::ExchangeLimit: return protocol::LoopStatus::exchange_limit;
        case model_io::LoopStatus::Failed: return protocol::LoopStatus::failed;
        case model_io::LoopStatus::AutoCompactRequired:
            return protocol::LoopStatus::auto_compact_required;
    }
    throw std::invalid_argument("invalid loop status");
}

protocol::LoopPhase wire_loop_phase(model_io::LoopPhase phase) {
    switch (phase) {
        case model_io::LoopPhase::Ready: return protocol::LoopPhase::ready;
        case model_io::LoopPhase::Model: return protocol::LoopPhase::model;
        case model_io::LoopPhase::Tools: return protocol::LoopPhase::tools;
        case model_io::LoopPhase::Projection: return protocol::LoopPhase::projection;
        case model_io::LoopPhase::Blocked: return protocol::LoopPhase::blocked;
    }
    throw std::invalid_argument("invalid loop phase");
}

/** Provider descriptors are owned metadata; they must match the published shape. */
std::vector<protocol::OptionChoice> choices(const Json& available) {
    std::vector<protocol::OptionChoice> result;
    if (!available.is_array()) return result;
    result.reserve(available.size());
    for (const auto& entry : available) {
        result.push_back(protocol::from_json<protocol::OptionChoice>(entry));
    }
    return result;
}

/** Recovery state as the wire reports it; pending results stay open JSON. */
protocol::LoopProgress loop_progress(const model_io::LoopProgress& loop) {
    protocol::LoopProgress value;
    value.status = wire_loop_status(loop.status);
    value.phase = wire_loop_phase(loop.phase);
    value.completed_exchanges = loop.completed_exchanges;
    value.committed_response_sequence = loop.committed_response_sequence;
    value.error = loop.error;
    value.pending_results = Json(loop.pending_results);
    return value;
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
        // Construction performs no I/O; a store exists even when persistence is
        // disabled so restore/admission checks stay in one place.
        if (config.persistence_backend == "sqlite") {
            state_store = persistence::make_sqlite_store(session_id, config.state_directory);
        } else if (config.persistence_backend == "json" || config.persistence_backend == "auto") {
            state_store = persistence::make_json_store(session_id, {
                config.state_directory, config.memory});
        } else {
            throw std::invalid_argument("unsupported persistence backend");
        }
    }

    asio::strand<asio::any_io_executor> strand;
    std::optional<persistence::Revision> storage_revision;
    load::Configuration config;
    std::string session_id;
    /** Authoritative state, compact archives and retention for this session. */
    std::unique_ptr<persistence::StateStore> state_store;
    // One storage worker serializes blocking database work. Declared after the
    // state it touches so destruction joins every queued task before config and
    // state_store disappear; all calls are joined before run() returns.
    asio::thread_pool storage_executor{1};
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
    /// Optional archive I/O can still make the authoritative store uncertain.
    std::optional<std::string> optional_storage_failure;
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

    /** Admission class of one event; a message type alone cannot decide this. */
    EventOutbox::Kind classify(protocol::EventName name, const Json& data) const {
        using protocol::EventName;
        switch (name) {
            case EventName::model_response:
            case EventName::tool_calls:
            case EventName::tool_results:
                return EventOutbox::Kind::Preview;
            case EventName::history:
            case EventName::answer:
            case EventName::history_error:
            case EventName::answer_error:
                return EventOutbox::Kind::Query;
            case EventName::persisted:
            case EventName::status:
            case EventName::options:
            case EventName::export_error:
            case EventName::error:
                // A terminal storage failure is a lifecycle boundary; every other
                // error is a replayable diagnostic.
                return storage_failed ? EventOutbox::Kind::Lifecycle : EventOutbox::Kind::Metadata;
            case EventName::compact_finished:
                // Automatic compaction is latest-value metadata; a manual result is
                // a lifecycle event the transcript must retain.
                return data.value("origin", std::string()) == "automatic"
                    ? EventOutbox::Kind::Metadata : EventOutbox::Kind::Lifecycle;
            case EventName::ready:
            case EventName::input_admitted:
            case EventName::run_started:
            case EventName::input_committed:
            case EventName::run_finished:
                return EventOutbox::Kind::Lifecycle;
            case EventName::input_rejected:
                break;
        }
        return EventOutbox::Kind::Feedback;
    }

    /** A described record is serialized; open event data is used as it is. */
    template <class Data>
    static Json wire_data(Data data) {
        if constexpr (protocol::DescribedRecord<Data>) {
            return protocol::to_json(data);
        } else {
            return data;
        }
    }

    /** Bounded projection events are trimmed by their producer, never here. */
    static bool is_projected(protocol::EventName name) {
        using protocol::EventName;
        return name == EventName::model_response || name == EventName::tool_calls
            || name == EventName::tool_results || name == EventName::history
            || name == EventName::answer;
    }

    /** Noncritical congestion is bounded and observable, never a run failure. */
    void publish(protocol::EventName name, Json data,
                 const std::string& event_request, const std::string& event_run) {
        const auto kind = classify(name, data);
        protocol::EventEnvelope envelope;
        envelope.event = name;
        envelope.session_id = session_id;
        envelope.worker_id = worker_id;
        envelope.request_id = event_request;
        envelope.run_id = event_run;
        envelope.data = std::move(data);
        const auto admission = outgoing.admit(std::move(envelope), kind, client.queued_write_bytes());
        if (admission == EventOutbox::Admission::Omitted && kind == EventOutbox::Kind::Lifecycle) {
            throw std::runtime_error("application lifecycle event admission failed");
        }
        if (admission != EventOutbox::Admission::Omitted) {
            outgoing_ready.try_send(boost::system::error_code{}, true);
        }
    }

    /** One event with explicit correlation; data may be a record or open JSON. */
    template <class Data>
    void emit(protocol::EventName name, Data data,
              const std::string& event_request, const std::string& event_run) {
        publish(name, wire_data(std::move(data)), event_request, event_run);
    }

    /** Ordinary events belong to the current run; rejected inputs never do. */
    template <class Data>
    void emit(protocol::EventName name, Data data) {
        Json value = wire_data(std::move(data));
        if (name == protocol::EventName::compact_finished) {
            value = display_compact(std::move(value));
        } else if (!is_projected(name)) {
            value = display_value(value);
        }
        publish(name, std::move(value), request_id, run_id);
    }

    /** One persistence receipt as the wire reports it. */
    static protocol::PersistedData persisted(persistence::Boundary boundary,
                                             const persistence::CommitReceipt& receipt) {
        protocol::PersistedData value;
        value.boundary = wire_boundary(boundary);
        value.backend = wire_backend(receipt.backend);
        if (receipt.revision) value.revision = *receipt.revision;
        value.operation_id = receipt.operation_id;
        return value;
    }

    /** Report a rejected request without borrowing the active run's identity. */
    void reject_input(protocol::InputRejectedData rejection) {
        const auto event_request = rejection.request_id.is_string()
            ? rejection.request_id.get<std::string>() : std::string();
        emit(protocol::EventName::input_rejected, std::move(rejection), event_request, "");
    }

    protocol::StatusData status() const {
        protocol::StatusData value;
        value.active = active;
        value.stopping = stopping;
        value.storage_failed = storage_failed;
        value.rejected_payloads = client.rejected_payloads();
        value.rejected_queries = client.rejected_queries();
        value.unreported_rejections = client.unreported_rejections();
        value.event_queue = protocol::EventQueue{outgoing.size(), outgoing.bytes(),
                                                 client.queued_write_bytes()};
        value.memory_retention = protocol::MemoryRetention{config.memory_retention.max_archives};
        value.auto_compact = compact_counters();
        value.capabilities = {protocol::Capability::session_history,
                              protocol::Capability::context_compact,
                              protocol::Capability::auto_compact,
                              protocol::Capability::answer_pages};
        if (state.loop) value.loop = loop_progress(*state.loop);
        return value;
    }

    /**
     * Read-only capability and current-selection snapshot. Empty reserved
     * categories do not imply that tools are disabled.
     */
    protocol::OptionsData options() const {
        const llm::LLMModel& provider = *driver_model;
        protocol::OptionsData value;
        value.model = protocol::OptionCategory{choices(provider.get_options()),
                                               provider.get_current_options()};
        value.tools = protocol::OptionCategory{{}, Json::object()};
        value.confirmation = protocol::OptionCategory{
            choices(confirmation_options.get_options()),
            confirmation_options.get_current_options()};
        return value;
    }

    /**
     * Publish the required snapshot and latch failure if it cannot be committed.
     *
     * A checkpoint returns only after the store knows whether the state is
     * durable, so a later tool dispatch can never run behind a write that is
     * still in flight. The checkpoint is the only automatic storage work: the
     * write-only Markdown view is no longer produced by the worker, and the
     * complete snapshot is authoritative regardless of its storage backend.
     */
    asio::awaitable<void> save(persistence::Boundary boundary) {
        if (!config.persistence || storage_failed) co_return;
        persistence::CommitReceipt receipt;
        try {
            if (state.meta.session_id != session_id)
                throw std::logic_error("hook changed the worker session identity");
            receipt = co_await asio::co_spawn(storage_executor,
                [this, boundary]() -> asio::awaitable<persistence::CommitReceipt> {
                    co_return state_store->checkpoint(state, boundary, storage_revision);
                }, asio::use_awaitable);
            storage_revision = receipt.revision;
        } catch (const persistence::StoreError&) {
            // Only a store failure latches storage; a programming error (for
            // example a hook mutating the session identity above) must stay
            // distinguishable and must not stop persistence for the run.
            storage_failed = true;
            if (!failure) failure = std::current_exception();
            cancel();
            throw;
        }
        if (boundary == persistence::Boundary::RunFinished
            || boundary == persistence::Boundary::Cancelled) run_saved = true;
        emit(protocol::EventName::persisted, persisted(boundary, receipt));
    }

    /** Initialize resources before admission; restored history is never replayed. */
    asio::awaitable<void> initialize() {
        if (config.legacy_readable) {
            // One startup diagnostic; the value stays accepted so an existing
            // Hub-generated configuration keeps starting unchanged.
            logging::Logger::warning(
                "persistence.readable is deprecated and ignored: the worker writes "
                "complete state only and no longer exports readable.md");
        }
        // Auto selection needs disk inspection before deciding which intrinsic
        // capabilities to expose. Restore first; rebuild prompts only after the
        // complete registry exists, so stale host skills are replaced together.
        co_await restore_state();
        construct_runtime();
        refresh_runtime_prompt();
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
        if (config.persistence && state_store->backend() == persistence::Backend::Sqlite) {
            registry.add(std::make_shared<tools::intrinsic::ArchiveMemoryToolSet>(
                persistence::make_sqlite_archive_reader(session_id, config.state_directory),
                storage_executor.get_executor()));
        }
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

    /**
     * Select the backend and restore history before constructing capabilities.
     *
     * A store that exists but cannot be decoded fails startup; it is never
     * treated as an empty session. `restore: never` still validates the store
     * but does not adopt its state; a later save replaces it in place.
     */
    asio::awaitable<void> restore_state() {
        std::optional<model_io::AgentInputState> stored;
        if (config.persistence) {
            auto current = co_await asio::co_spawn(storage_executor,
                [this]() -> asio::awaitable<std::optional<persistence::StoredState>> {
                    const auto backend = persistence::select_backend(
                        config.state_directory, config.persistence_backend);
                    if (backend == persistence::Backend::Sqlite) {
                        state_store = persistence::make_sqlite_store(session_id, config.state_directory);
                    } else {
                        state_store = persistence::make_json_store(session_id,
                            {config.state_directory, config.memory});
                    }
                    // Even restore: never validates existing storage and identity.
                    // It starts fresh only after proving this is the selected store.
                    auto current = state_store->load_current();
                    if (current && current->state.meta.session_id != session_id) {
                        throw std::runtime_error("stored session ID does not match selected session");
                    }
                    co_return current;
                }, asio::use_awaitable);
            if (current) {
                storage_revision = current->revision;
                if (config.restore) stored = std::move(current->state);
            }
        }
        if (stored) {
            state = std::move(*stored);
            if (state.meta.session_id != session_id)
                throw std::runtime_error("restored session ID does not match selected session");
        } else {
            state.meta.session_id = session_id;
            state.meta.created_at = persistence::utc_timestamp();
            state.meta.updated_at = state.meta.created_at;
            state.system_prompt = std::move(config.system_prompt);
        }
    }

    /** Refresh model-facing tools and host sections after backend selection and
     * runtime construction. Historical conversation and compact memory survive. */
    void refresh_runtime_prompt() {
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
                emit(protocol::EventName::input_committed, protocol::EmptyData{});
            }
        }));
        subscriptions.emplace_back(events.subscribe<loop::ModelCommitted>([this](const auto& event) {
            ++history_revision;
            Json projected = project_response(event.state, event.state.turns.size() - 1,
                event.state.turns.back().agent_loop_step.size() - 1, worker_id);
            emit(protocol::EventName::model_response, std::move(projected));
        }));
        subscriptions.emplace_back(events.subscribe<loop::BeforeToolBatch>([this](const auto& event) {
            emit(protocol::EventName::tool_calls, display_calls(event.calls));
        }));
        subscriptions.emplace_back(events.subscribe<loop::ToolResultsCommitted>([this](const auto& event) {
            emit(protocol::EventName::tool_results, display_results(*event.state.turns.back().agent_loop_step.back().invoke_returns));
        }));
        // Metadata edits belong to writable transactions, never to the
        // read-only checkpoint observers. Recovery snapshots retain the last
        // logical edit timestamp rather than mutating state during publication.
        subscriptions.emplace_back(events.subscribe<loop::EditOnStepFinished>([](const auto& event) {
            event.state.meta.updated_at = persistence::utc_timestamp();
        }));
        subscriptions.emplace_back(events.subscribe<loop::EditOnRunFinished>([](const auto& event) {
            event.state.meta.updated_at = persistence::utc_timestamp();
        }));
        subscriptions.emplace_back(events.subscribe<loop::StepFinished>([this](const auto&) {
            ++history_revision;
        }));
        subscriptions.emplace_back(events.subscribe<loop::RunFinished>([this](const auto&) {
            ++history_revision;
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
                    emit(protocol::EventName::answer, answer_page(state, payload, worker_id));
                } else {
                    auto request = parse_history_request(payload);
                    emit(protocol::EventName::history, history_page(state, request, history_revision, worker_id));
                }
            } catch (const std::exception& error) {
                emit(answer ? protocol::EventName::answer_error : protocol::EventName::history_error,
                    protocol::QueryErrorData{payload.value("request_id", Json()), error.what()});
            }
        }));
        subscriptions.emplace_back(events.subscribe<io::PayloadQueryRejectedEvent>([this](const auto& event) {
            emit(event.operation == "answer" ? protocol::EventName::answer_error
                                          : protocol::EventName::history_error,
                protocol::QueryErrorData{event.request_id,
                    "Worker query quota exceeded; retry after the connection drains."});
        }));
        subscriptions.emplace_back(events.subscribe<io::PayloadRejectedEvent>([this](const auto& event) {
            if (stopping || shutdown_requested.load()) return;
            protocol::InputRejectedData rejection;
            rejection.request_id = event.request_id;
            rejection.message = "Worker input queue is full. Wait for current work to finish, then retry.";
            rejection.code = protocol::RejectionCode::payload_queue_full;
            if (event.operation.is_string()) {
                const std::string operation = event.operation;
                rejection.operation = input_operation(operation);
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
                protocol::SignalRequest request;
                try {
                    request = protocol::from_json<protocol::SignalRequest>(signal);
                } catch (const protocol::ValidationError& error) {
                    throw std::invalid_argument(error.what());
                }
                switch (request.operation) {
                    case protocol::SignalOperation::cancel:
                        if (!request.run_id || request.run_id->empty()) {
                            throw std::invalid_argument("cancel requires run_id");
                        }
                        emit(protocol::EventName::status, status());
                        break;
                    case protocol::SignalOperation::status:
                        emit(protocol::EventName::status, status());
                        break;
                    case protocol::SignalOperation::options:
                        emit(protocol::EventName::options, options());
                        break;
                    case protocol::SignalOperation::shutdown:
                        // Shutdown is handled before the bounded control mailbox.
                        throw std::invalid_argument("unknown signal operation");
                }
            } catch (const std::exception& error) {
                emit(protocol::EventName::error, protocol::ErrorData{error.what()});
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

    /**
     * Map the archive directories this retained state names to store records.
     *
     * Historical states name absolute archive paths. Only the store can decide
     * whether such a path still identifies an archive it owns, so the worker
     * never protects a path by itself.
     */
    std::vector<persistence::ArchiveRef> archive_references(
        const model_io::AgentInputState& retained,
        const std::filesystem::path& root) const {
        std::vector<persistence::ArchiveRef> references;
        for (const auto& path : compact_archive_references(retained, root)) {
            if (auto reference = state_store->archive_at(path))
                references.push_back(std::move(*reference));
        }
        return references;
    }

    /** Protect authoritative references and any replacement whose publication
     * may have succeeded before a required snapshot write reported failure.
     */
    persistence::CleanupResult cleanup_archive(
        const std::filesystem::path& root,
        const persistence::ArchiveRef& current,
        std::span<const persistence::ArchiveRef> replacement_references = {}
    ) {
        persistence::ArchivePruneRequest request;
        request.current = current;
        request.referenced = archive_references(state, root);
        request.referenced.insert(request.referenced.end(),
            replacement_references.begin(), replacement_references.end());
        request.policy = config.memory_retention;
        return state_store->prune_archives(request);
    }

    /** Keep the host and store failure latches consistent after optional I/O.
     * A definitely uncommitted cleanup remains a warning. An uncertain commit
     * forbids all further writes and admission, but transport stays alive until
     * consume() emits the terminal error carrying the original compact outcome.
     */
    void latch_optional_storage_failure(const persistence::StoreError& error) {
        if (error.status() != persistence::CommitStatus::Unknown) return;
        storage_failed = true;
        if (!failure) failure = std::current_exception();
        if (!optional_storage_failure) optional_storage_failure = error.what();
        cancel();
    }

    /**
     * Settle a failed or cancelled attempt, then apply retention.
     *
     * Export errors remain optional: report through bounded events and logging,
     * even if the transport cannot admit the notification. The caller contains
     * diagnostic failures so bookkeeping never replaces the primary outcome.
     * Each storage operation is awaited; no cleanup outlives this attempt.
     */
    asio::awaitable<void> settle_failed_compact(
        const std::filesystem::path& root,
        const persistence::ArchiveRef& current,
        persistence::ArchiveOutcome outcome,
        const std::vector<persistence::ArchiveRef>& replacement_references
    ) {
        // A database attempt is settled and trimmed only when its commit state
        // is known; an uncertain SQLite commit keeps rows as recovery evidence.
        // JSON cleanup after an uncertain publish is safe because it protects
        // the union of both candidate states below.
        if (storage_failed && state_store->backend() == persistence::Backend::Sqlite) co_return;
        try {
            co_await asio::co_spawn(storage_executor,
                [&]() -> asio::awaitable<void> {
                    state_store->finish_archive_attempt(current, outcome);
                    co_return;
                }, asio::use_awaitable);
        } catch (const persistence::StoreError& error) {
            latch_optional_storage_failure(error);
            logging::Logger::warning("compact archive settle failed: {}", error.what());
        } catch (const std::exception& error) {
            logging::Logger::warning("compact archive settle failed: {}", error.what());
        }
        // An uncertain settlement must not be followed by another mutation.
        if (optional_storage_failure
            || (storage_failed && state_store->backend() == persistence::Backend::Sqlite)) co_return;
        try {
            co_await asio::co_spawn(storage_executor,
                [&]() -> asio::awaitable<void> {
                    cleanup_archive(root, current, replacement_references);
                    co_return;
                }, asio::use_awaitable);
        } catch (const persistence::StoreError& error) {
            latch_optional_storage_failure(error);
            logging::Logger::warning("compact archive cleanup failed: {}", error.what());
            emit(protocol::EventName::export_error,
                protocol::ExportErrorData{"archive_cleanup", error.what()});
        } catch (const std::exception& error) {
            logging::Logger::warning("compact archive cleanup failed: {}", error.what());
            emit(protocol::EventName::export_error,
                protocol::ExportErrorData{"archive_cleanup", error.what()});
        }
    }

    /** Own an attempt across all coroutine exits. Await settlement outside catch
     * blocks so failed/cancelled summaries do not leave background writes alive.
     * Cleanup is best effort and cannot replace the primary result or exception.
     */
    asio::awaitable<loop::RunResult> compact(
        CompactMode mode, const std::string& call_id = {}) {
        if (run_stop.stop_requested()) {
            loop::RunResult result;
            result.status = loop::RunStatus::Cancelled;
            co_return result;
        }
        const auto memory_directory = std::filesystem::absolute(config.memory).lexically_normal();
        // Reservation touches the archive directory for the JSON backend, so it
        // runs on the storage executor like every other store call.
        const auto archive = co_await asio::co_spawn(storage_executor,
            [&]() -> asio::awaitable<persistence::ArchiveRef> {
                co_return state_store->reserve_archive(
                    {run_id, mode == CompactMode::Automatic, call_id});
            }, asio::use_awaitable);
        CompactPlan plan;
        std::exception_ptr preflight_failure;
        try {
            plan = plan_compact(state, config.storage / compact_memory_filename);
        } catch (...) {
            preflight_failure = std::current_exception();
        }
        if (preflight_failure) {
            // Preflight rejected the attempt before T1: drop the in-memory
            // reservation so a long-lived worker does not accumulate identities.
            co_await asio::co_spawn(storage_executor,
                [&]() -> asio::awaitable<void> {
                    state_store->release_archive(archive);
                    co_return;
                }, asio::use_awaitable);
            std::rethrow_exception(preflight_failure);
        }
        try {
            const auto prepared = co_await asio::co_spawn(storage_executor,
                [&]() -> asio::awaitable<persistence::CommitReceipt> {
                    co_return state_store->begin_archive(archive, state, storage_revision);
                }, asio::use_awaitable);
            storage_revision = prepared.revision;
        } catch (const persistence::StoreError& error) {
            if (state_store->backend() == persistence::Backend::Sqlite
                || error.status() == persistence::CommitStatus::Unknown) {
                storage_failed = true;
                if (!failure) failure = std::current_exception();
                cancel();
            }
            throw;
        }
        loop::RunResult result;
        std::exception_ptr primary;
        bool committed = false;
        std::vector<persistence::ArchiveRef> replacement_references;
        std::unique_ptr<fileio::StagedWrite> staged_memory;
        try {
            result = co_await compact_attempt(mode, call_id, archive, memory_directory,
                std::move(plan), committed, replacement_references, staged_memory);
        } catch (...) {
            primary = std::current_exception();
        }
        // Join temporary cleanup on the storage executor on every normal exit,
        // including cancellation and uncertain state commits. Never detach I/O.
        co_await asio::co_spawn(storage_executor, [&]() -> asio::awaitable<void> {
            staged_memory.reset();
            co_return;
        }, asio::use_awaitable);
        if (!committed) {
            const auto outcome = !primary && result.status == loop::RunStatus::Cancelled
                ? persistence::ArchiveOutcome::Cancelled : persistence::ArchiveOutcome::Failed;
            try {
                co_await settle_failed_compact(memory_directory, archive, outcome, replacement_references);
            } catch (...) {
                // Includes logging/transport failures: retain the primary outcome.
            }
        }
        if (primary) std::rethrow_exception(primary);
        co_return result;
    }

    /**
     * Summarize a private conversation copy, then publish a durable replacement.
     * The ordinary hook bus and automatic saves never observe the temporary
     * compact turn. History queries continue to see the original state while
     * the model is suspended. A fresh local bus prohibits tool dispatch even
     * when a provider ignores the omitted tool definitions and prompt.
     *
     * Archive identities are exclusively reserved and never reused. Successful
     * and failed attempts remain in persistent sequence order for inspection.
     * Every settled archived attempt applies retention, protecting the current
     * evidence and references in retained state. Preflight creates no archive.
     * The store commit is the boundary: before it reports success the live state
     * is untouched, and a committed replacement settles its own attempt. A
     * published-but-unsynced write stops the worker just like other required
     * snapshot failures. Write failures retain archive references from both the
     * old state and the attempted replacement because publication may already
     * have occurred. Cancellation during the synchronous commit does not roll
     * back an already published snapshot.
     */
    asio::awaitable<loop::RunResult> compact_attempt(
        CompactMode mode, const std::string& call_id,
        const persistence::ArchiveRef& archive,
        const std::filesystem::path& memory_directory,
        CompactPlan plan, bool& committed,
        std::vector<persistence::ArchiveRef>& replacement_references,
        std::unique_ptr<fileio::StagedWrite>& staged_memory) {
        const auto stop = run_stop.get_token();
        if (stop.stop_requested()) {
            loop::RunResult result;
            result.status = loop::RunStatus::Cancelled;
            co_return result;
        }
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
        content.raw += "\n\nSummary preview: approximately the first "
            + std::to_string(plan.body_bytes)
            + " UTF-8 bytes (not tokens or JSON-escaped bytes) are injected into the next run. "
              "The complete summary is preserved in memory.md even when the preview is truncated. "
              "Front-load the user's goals, preferences and rules, current task status, important "
              "files/processes, findings or how to recover them, and concrete next steps.";
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
        // replacement commit begins, cancellation still preserves the old state.
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
        replacement.meta.updated_at = persistence::utc_timestamp();
        replacement.tools = state.tools;
        replacement.extras = state.extras;
        model_io::sync_external_status(replacement,
            loop::intrinsic::ContextStatisticHook::kName,
            model_io::external_status(draft, loop::intrinsic::ContextStatisticHook::kName).value());
        replacement.loop = std::move(draft.loop);
        const auto hard_target = plan.hard_target;
        auto preview = compact_prompt(std::move(plan), summary);
        replacement.system_prompt = std::move(preview.prompt);
        const auto after_bytes = compact_context_bytes(replacement);
        if (after_bytes > hard_target) {
            throw std::runtime_error("compact replacement exceeds byte budget: replacement_bytes="
                + std::to_string(after_bytes) + ", budget_bytes="
                + std::to_string(hard_target));
        }
        // Refresh prompt-size estimates after memory injection; the reconciled
        // checkpoint prevents this second update from counting the cost twice.
        compact_events.publish(loop::EditOnRunFinished{replacement, result});
        const auto removed_turns = state.turns.size();
        // Allocate the success event before committing so construction failures
        // cannot be mistaken for an uncommitted operation.
        protocol::CompactFinishedData completed;
        completed.summary = std::move(preview.summary);
        completed.archive = protocol::ArchiveRef{archive.session_id, archive.archive_id,
                                                 wire_backend(archive.backend), archive.restorable()};
        completed.removed_turns = removed_turns;
        completed.revision = history_revision + 1;
        completed.durable = true;
        if (!archive.payload.empty()) completed.memory_file = archive.payload.string();
        if (mode == CompactMode::Automatic) {
            completed.origin = "automatic";
            completed.cycle = compact_attempts;
            completed.call_id = call_id;
        }
        if (stop.stop_requested()) {
            result.status = loop::RunStatus::Cancelled;
            co_return result;
        }
        // A failed parent-directory sync leaves the replacement visible on
        // disk while live state remains old. Protect the union on every failed
        // commit attempt, conservatively including failures before publication.
        replacement_references = co_await asio::co_spawn(storage_executor,
            [&]() -> asio::awaitable<std::vector<persistence::ArchiveRef>> {
                co_return archive_references(replacement, memory_directory);
            }, asio::use_awaitable);
        // Stage the complete model output before the authoritative commit. A
        // failed attempt never publishes its file; the outer coroutine owns
        // cleanup so even exception/cancellation paths await filesystem work.
        const auto memory_file = config.storage / compact_memory_filename;
        co_await asio::co_spawn(storage_executor, [&]() -> asio::awaitable<void> {
            if (std::filesystem::is_symlink(std::filesystem::symlink_status(memory_file))) {
                throw std::runtime_error("compact memory file must not be a symlink");
            }
            staged_memory = std::make_unique<fileio::StagedWrite>(memory_file,
                [&](std::ostream& output) { output << summary; });
            co_return;
        }, asio::use_awaitable);
        if (stop.stop_requested()) {
            result.status = loop::RunStatus::Cancelled;
            co_return result;
        }
        persistence::CommitReceipt receipt;
        try {
            receipt = co_await asio::co_spawn(storage_executor,
                [&]() -> asio::awaitable<persistence::CommitReceipt> {
                    co_return state_store->commit_compaction(archive, replacement, storage_revision);
                }, asio::use_awaitable);
        } catch (const persistence::StoreError&) {
            storage_failed = true;
            if (!failure) failure = std::current_exception();
            cancel();
            throw;
        }
        static_assert(std::is_nothrow_move_assignable_v<model_io::AgentInputState>);
        state = std::move(replacement);
        committed = true;
        storage_revision = receipt.revision;
        ++history_revision;
        run_saved = true;
        // State and memory.md are separate publications. Do not cancel between
        // them or roll back a committed state if file publication fails. A crash
        // here may leave the previous file; the bounded preview remains usable.
        try {
            co_await asio::co_spawn(storage_executor, [&]() -> asio::awaitable<void> {
                if (std::filesystem::is_symlink(std::filesystem::symlink_status(memory_file))) {
                    throw std::runtime_error("compact memory file must not be a symlink");
                }
                staged_memory->publish();
                staged_memory.reset();
                co_return;
            }, asio::use_awaitable);
        } catch (...) {
            // Keep peer-visible compact protocol unchanged and diagnostics bounded.
            try {
                logging::Logger::warning("compact committed, but memory.md publication failed; full summary may be stale or unavailable");
            } catch (...) { /* Diagnostics cannot change the committed result. */ }
        }
        // A durable commit settles its own attempt, so success is never a second
        // fallible step. The archive itself stays immutable evidence.
        emit(protocol::EventName::persisted,
            persisted(persistence::Boundary::Compaction, receipt));
        // Retention sees the committed replacement. The outer coroutine handles
        // failed/cancelled attempts against the unchanged authoritative state.
        try {
            const auto cleaned = co_await asio::co_spawn(storage_executor,
                [&]() -> asio::awaitable<persistence::CleanupResult> {
                    co_return cleanup_archive(memory_directory, archive);
                }, asio::use_awaitable);
            completed.archive_cleanup = protocol::ArchiveCleanup{
                cleaned.removed_archives, cleaned.removed_bytes};
        } catch (const persistence::StoreError& error) {
            latch_optional_storage_failure(error);
            completed.archive_cleanup_error = error.what();
        } catch (const std::exception& error) {
            completed.archive_cleanup_error = error.what();
        }
        emit(protocol::EventName::compact_finished, std::move(completed));
        co_return result;
    }

    /** Checked counters cannot silently wrap the final wire exchange total. */
    static void add_exchanges(std::size_t& total, std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() - total) {
            throw std::overflow_error("logical run exchange counter exhausted");
        }
        total += count;
    }

    protocol::AutoCompact compact_counters() const {
        return {compact_attempts, compact_successes, config.max_auto_compactions,
                config.auto_compact_threshold};
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
                {config.max_exchanges, config.auto_compact_threshold,
                    [this](loop::Checkpoint boundary) -> asio::awaitable<void> {
                        switch (boundary) {
                            case loop::Checkpoint::BeforeTools:
                                co_await save(persistence::Boundary::BeforeTools);
                                break;
                            case loop::Checkpoint::ResultsReady:
                                co_await save(persistence::Boundary::ResultsReady);
                                break;
                            case loop::Checkpoint::StepFinished:
                                if (config.save_step) co_await save(persistence::Boundary::StepFinished);
                                break;
                            case loop::Checkpoint::RunFinished:
                                if (config.save_run) {
                                    try {
                                        co_await save(persistence::Boundary::RunFinished);
                                    } catch (const persistence::StoreError&) {
                                        // save() already latched storage_failed and
                                        // requested cancellation. Keep the loop's own
                                        // outcome: the terminal error below reports it
                                        // as primary_outcome instead of rewriting it to
                                        // Failed, and run_owned still rethrows failure.
                                    }
                                }
                                break;
                        }
                    }}, run_stop.get_token());
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
            emit(protocol::EventName::tool_calls, display_calls({query}));
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
            emit(protocol::EventName::tool_results, display_results({returned}));
            if (result.status != loop::RunStatus::Completed || storage_failed) {
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
        emit(protocol::EventName::ready, status());
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
                protocol::InputRejectedData rejection;
                rejection.request_id = payload.is_object()
                    ? payload.value("request_id", Json()) : Json();
                rejection.message = error.what();
                // Keep the requested operation in the replayable event. Hub
                // request records can expire before the transcript does.
                if (payload.is_object() && payload.contains("operation")
                    && payload.at("operation").is_string()) {
                    rejection.operation =
                        input_operation(payload.at("operation").get<std::string>());
                }
                if (applying_options || dynamic_cast<const InputOptionsError*>(&error)) {
                    rejection.code = protocol::RejectionCode::invalid_options;
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
                state.meta.updated_at = persistence::utc_timestamp();
            }
            run_saved = false;
            task_exchanges = compact_exchanges = compact_attempts = compact_successes = 0;
            failure_operation = input->operation == InputOperation::Compact ? "compact" : "task";
            emit(protocol::EventName::input_admitted,
                protocol::InputAdmittedData{wire_operation(input->operation)});
            emit(protocol::EventName::run_started, protocol::EmptyData{});
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
            // Early exits may not reach the loop's terminal durability barrier.
            // Save the settled state before reporting completion/admitting input,
            // including cancellation when ordinary run saves are disabled.
            if (input->operation != InputOperation::Compact && !storage_failed && !run_saved
                && (config.save_run || result.status == loop::RunStatus::Cancelled)) {
                try {
                    co_await save(config.save_run
                        ? persistence::Boundary::RunFinished
                        : persistence::Boundary::Cancelled);
                } catch (const persistence::StoreError&) {
                    // Latched by save(). This used to escape consume() and stop the
                    // transport without any terminal diagnostic; the storage_failed
                    // branch below now reports the outcome consistently.
                }
            }
            active = false;
            {
                std::lock_guard lock(control_mutex);
                scope.reset();
            }
            if (storage_failed) {
                // Preserve the last known recovery file; never overwrite failure evidence.
                stopping = true;
                protocol::ErrorData diagnostic;
                diagnostic.message = optional_storage_failure
                    ? "archive storage outcome is uncertain; worker is stopping"
                    : "required snapshot failed; worker is stopping";
                diagnostic.durable = false;
                // An opaque record: the superseded outcome keeps its own shape.
                diagnostic.primary_outcome = Json{{"status", run_status(result.status)},
                    {"error", result.error}, {"operation", failure_operation}};
                if (optional_storage_failure) diagnostic.storage_error = *optional_storage_failure;
                diagnostic.rejected_queries = client.rejected_queries();
                diagnostic.unreported_rejections = client.unreported_rejections();
                emit(protocol::EventName::error, std::move(diagnostic));
            } else {
                protocol::RunOutcomeData finished;
                finished.status = wire_status(result.status);
                finished.error = result.error;
                finished.exchanges = result.completed_exchanges;
                finished.task_exchanges = task_exchanges;
                finished.compact_exchanges = compact_exchanges;
                finished.auto_compact = compact_counters();
                finished.durable = run_saved;
                if (result.status == loop::RunStatus::Failed) {
                    finished.failure = protocol::FailureData{
                        wire_stage(result.failure_stage), failure_operation,
                        can_continue_memory() || (!state.turns.empty() && state.loop
                            && state.loop->phase == model_io::LoopPhase::Ready)};
                }
                // The terminal settlement also reports what this client dropped.
                finished.rejected_queries = client.rejected_queries();
                finished.unreported_rejections = client.unreported_rejections();
                emit(protocol::EventName::run_finished, std::move(finished));
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
        co_await initialize();
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
                state.meta.updated_at = persistence::utc_timestamp();
                co_await save(persistence::Boundary::Shutdown);
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
