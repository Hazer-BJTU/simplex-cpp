#pragma once

/**
 * @file llm/models.hpp
 * @brief Shared plugin ABI between the llm host and model-provider plugins.
 *
 * This header is the *contract* — compiled into both the host and every model
 * plugin (`.so`/`.dll`). It defines the model-type vocabulary, the model
 * interface (one class per stage of one model invocation), the plugin
 * descriptor, and the host-side dispatcher. It deliberately contains NO
 * concrete provider: an implementation (interpreter + SSE handler + reader +
 * endpoint wiring) is a plugin library of its own, following the
 * indextools/lang_plugin.hpp pattern this file mirrors.
 *
 * ## The three responsibilities of a model (and nothing else)
 *
 * A model instance is a *client bound to one provider configuration* — not an
 * agent, not a loop, not storage. Its interface covers the three stages of
 * one model invocation, plus the two runtime services every host needs from
 * such a client: the provider's live catalogue (provider_info) and the
 * generation knobs' adjustment (set_generation):
 *
 *   1. **Input translation** — model_io::AgentInputState (plus its stored
 *      config) into a provider request, via the plugin's own
 *      endpoint::ModelRequestInterpreter.
 *   2. **Request & response** — drive the whole exchange (interpreter ->
 *      sse_request -> reader) through endpoint::complete. This machinery is
 *      templated on the plugin's private delta type, so it must live entirely
 *      INSIDE the plugin library; the only signature crossing the DSO
 *      boundary is the type-erased converse() below.
 *   3. **Result integration** — how one finished model_io::MessageItem folds
 *      back into the conversation state (integrate()), including the
 *      provider's retention quirks (e.g. reasoning_content that must survive
 *      in replayed history). Persistence is only "the folded state stays
 *      serialisable through model_io's to_json"; the model never touches
 *      storage itself.
 *
 * Agent-loop / user-loop state management, tool EXECUTION, and the process-
 * level event queue are the HOST's concerns. Loop-is-a-process: streaming
 * observation will route through a process-wide event sink the plugin finds
 * in build() (e.g. binding its SSE handler to a global singleton), which is
 * why the lifecycle hooks below exist.
 *
 * ## Model types
 *
 * LLMModelType classifies by the SHAPE of a type's input/output dataclass,
 * not by marketing modality (multimodal *understanding* is still
 * Conversation — model_io::Content already carries binary / external-ref
 * payloads). Only Conversation is implemented today; each further type lands
 * as its own exchange entry point APPENDED to LLMModel (never a change to an
 * existing signature), guarded by the same default-throw body, and gated on
 * its model_io dataclass existing first.
 *
 * ## Two exported aliases per plugin
 *
 * A plugin `.so` is loaded through the generic extension_framework. It
 * exports TWO factory aliases:
 *
 *   - `create_llm_plugin` -> std::unique_ptr<extension::ExtensionContext>
 *       Mints the long-lived, stateless LLMModelExtensionContext descriptor
 *       (identity + warm() factory cache). Returned as the base context so
 *       the generic loader needs no llm-specific type.
 *
 *   - `create_llm_model`   -> std::unique_ptr<LLMModel>(
 *                                 boost::asio::any_io_executor,
 *                                 const nlohmann::json&)
 *       Mints a configured model instance. UNLIKE the lang-plugin product
 *       factory (nullary), this alias is SIGNED: the executor and the config
 *       JSON are the injection the protected LLMModel constructor requires,
 *       so construction and configuration are one atomic step. The framework's
 *       nullary product_factory cannot carry them, which is why
 *       LLMModelExtensionContext resolves this alias with its own signature.
 *
 * @code
 *   std::unique_ptr<extension::ExtensionContext> create_llm_plugin() {
 *       return std::make_unique<MyProviderPlugin>();
 *   }
 *   std::unique_ptr<llm::LLMModel> create_llm_model(
 *           boost::asio::any_io_executor executor, const nlohmann::json& config) {
 *       return std::make_unique<MyProviderModel>(std::move(executor), config);
 *   }
 *   BOOST_DLL_ALIAS(myprovider::create_llm_plugin, create_llm_plugin)
 *   BOOST_DLL_ALIAS(myprovider::create_llm_model,   create_llm_model)
 * @endcode
 *
 * ## The config JSON
 *
 * One JSON object is the model's entire configuration — there is no separate
 * endpoint parameter. Canonical shape:
 *
 *   {
 *     "endpoint": { "base_url": "...", "request_path": "...",
 *                   "auth": { "scheme": "...", "api_key": "..." } },
 *     "model": "provider-model-name",
 *     "...": "provider-free keys: temperature, stream, retry, effort, ..."
 *   }
 *
 * The "endpoint" value is exactly model_io::ModelEndpoint's serialised shape
 * (dataclass/endpoint_config.hpp); a plugin parses it inside build() via the
 * existing from_json, and a parse failure is an initialisation failure
 * (build() returns false). Everything past "endpoint" is provider-defined;
 * the host's config file passes the whole object through verbatim.
 *
 * The model derives its generation knobs from that config in build() —
 * everything except the host-owned "endpoint"/"provider"/"retry" keys.
 * set_generation() then adjusts those knobs at runtime (merge-patch style,
 * or the typed GenerationPreset for the model + reasoning-effort pair),
 * never touching the stored config itself; generation() reads them back.
 *
 * ## Lifetime
 *
 * Descriptor and every minted model live inside the loaded plugin library
 * (vtable + destructor + operator new all belong to the module). The
 * descriptor keeps its own library reference (bound by the loader). A minted
 * model's shared_ptr deleter captures the library handle, calls release(),
 * and only then deletes the object — so each model is safely destructible
 * independently, and release() always runs on the complete (most-derived)
 * object even though destruction-order tricks would normally make that
 * impossible.
 *
 * ## ABI versioning
 *
 * llm::LLM_PLUGIN_ABI_VERSION (rendered by versioning/) is checked by
 * LLMDispatcher before any model is minted. Host and plugins are always
 * rebuilt together from the same headers; a stale plugin is rejected at load.
 */

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/dll/shared_library.hpp>
#include <nlohmann/json.hpp>

#include "dataclass/model_io.hpp"
#include "extension_framework/extensions.hpp"
#include "versioning/version.hpp"

namespace llm {

/// Bump whenever LLMModel or LLMModelExtensionContext changes in a
/// binary-incompatible way. The canonical value lives in versioning/;
/// this alias keeps llm-namespace references short.
inline constexpr std::uint32_t LLM_PLUGIN_ABI_VERSION = simplex::LLM_PLUGIN_ABI_VERSION;

/// The BOOST_DLL_ALIAS name the host imports to mint the descriptor.
inline constexpr const char* LLM_PLUGIN_FACTORY_NAME = "create_llm_plugin";

/// The BOOST_DLL_ALIAS name the host imports (cached by warm()) to mint
/// configured model instances. Signed: (executor, config) -> unique_ptr<LLMModel>.
inline constexpr const char* LLM_MODEL_FACTORY_NAME = "create_llm_model";

/**
 * @brief The kind of model, by the shape of its input/output dataclass.
 *
 * Classified by data shape, not modality: multimodal understanding is still
 * Conversation. Only Conversation is implemented; the others are reserved
 * slots for their future model_io dataclasses. APPEND new values at the tail
 * only (the underlying value is part of the plugin ABI: renumbering or
 * inserting reorders every wire/serialised encoding).
 */
enum class LLMModelType : std::uint8_t {
    Conversation,    // chat-completion style exchange: AgentInputState -> MessageItem.
    Generation,      // plain text completion (legacy completions / codegen).
    Embedding,       // inputs -> vectors.
    Rerank,          // query + candidates -> scored ordering.
    ImageGeneration, // prompt (+/ image edits) -> images.
    VideoGeneration, // prompt (+/ image anchors) -> video.
    Speech,          // text -> audio (TTS).
    Transcription,   // audio -> text (STT).
    Moderation,      // content -> policy verdicts.
    Others,          // escape hatch for shapes not yet named.
};

/**
 * @brief A model was asked to perform an exchange its type does not implement.
 *
 * Thrown by LLMModel's default exchange bodies — i.e. the model instance
 * (or model type) does not override the entry point being called. Distinct
 * from provider/transport failures (endpoint::HttpRequestException), which
 * mean "tried and failed": this means "not this model's job".
 */
class LLMUnsupportedOperation : public std::logic_error {
public:
    explicit LLMUnsupportedOperation(const std::string& what_arg)
        : std::logic_error(what_arg) {}
};

/**
 * @brief The thinking-effort knob's cross-provider common vocabulary.
 *
 * The typed tier of set_generation() covers the levels every major provider
 * agrees on; provider-private spellings beyond them (e.g. DeepSeek's
 * "xhigh"/"max") go through the JSON merge-patch tier verbatim and are the
 * provider's business. The wire spelling is the lowercase name.
 *
 * APPEND-ONLY like LLMModelType: the underlying value order is part of the
 * plugin ABI (it crosses the DSO boundary inside GenerationPreset).
 */
enum class ReasoningEffort : std::uint8_t {
    Minimal,
    Low,
    Medium,
    High,
};

/// The lowercase wire spelling of one effort level ("minimal", ...).
inline constexpr std::string_view to_string(ReasoningEffort effort) noexcept {
    switch (effort) {
        case ReasoningEffort::Minimal: return "minimal";
        case ReasoningEffort::Low:     return "low";
        case ReasoningEffort::Medium:  return "medium";
        case ReasoningEffort::High:    return "high";
    }
    return "minimal";
}

/**
 * @brief The typed tier of set_generation(): the pair of knobs every provider
 *        model shares and that hosts adjust most often.
 *
 * Which model to run, and how hard it may think. Both fields are optional —
 * nullopt leaves that knob untouched, so a preset may change either, both,
 * or nothing. The canonical spelling the contract writes is the
 * "reasoning": {"effort": ...} envelope, which both protocol adapters
 * already translate (chat-completions interpreters fold it into
 * reasoning_effort; the Responses API carries it natively).
 */
struct GenerationPreset {
    /// The provider model name (the "model" key). An empty string is
    /// rejected by the shared validation core.
    std::optional<std::string> model;
    /// The thinking-effort level (the "reasoning"/"effort" envelope).
    std::optional<ReasoningEffort> effort;
};

/**
 * @brief One model instance: a client bound to one provider configuration.
 *
 * The interface covers the three stages in the file header — input
 * translation, the exchange itself, result integration — and nothing else.
 * The host owns every loop; the model never executes tools, never stores
 * anything, never manages conversation lifecycle beyond folding results in.
 *
 * Construction is privileged: the (executor, config) constructor is protected
 * so instances exist only through a plugin's create_llm_model alias (or an
 * in-memory subclass in tests). The lifecycle below is then guaranteed by
 * LLMModelExtensionContext::create_model — hosts never call build/release by
 * hand.
 *
 * Lifecycle (guaranteed call order, driven by create_model):
 *   1. construction (executor + config injected atomically)
 *   2. build() exactly once, before any use — virtual initialisation the
 *      constructor cannot do: parse/validate the endpoint out of the config,
 *      locate process-level singletons (the event sink) and bind them to the
 *      plugin's SSE handler, warm caches. Returning false (or throwing,
 *      though it is noexcept) means initialisation failed: the model is
 *      destroyed and create_model reports failure. Idempotent by contract —
 *      a second call must be harmless.
 *   3. use — converse()/integrate() any number of times.
 *   4. release() exactly once, immediately before destruction. Virtual
 *      teardown mirroring build(): unbind event sources, flush state.
 *      noexcept; runs on the COMPLETE object (the deleter calls it before
 *      delete), so overrides may touch their own members freely. After
 *      release() the object must not be used again.
 *
 * Concurrency: one instance serves one conversation sequentially. State set
 * in build() is immutable afterwards — the one deliberate exception being
 * the generation knobs, which set_generation() mutates between exchanges
 * (never concurrent with an in-flight converse()/provider_info());
 * converse() does I/O through the stored executor. Thread-safety of
 * concurrent converse() calls is the plugin's choice and not guaranteed by
 * this contract.
 */
class LLMModel {
public:
    virtual ~LLMModel() = default;

    /// The shape this model implements; drives host-side routing and
    /// capability listing. The only pure-virtual member.
    virtual LLMModelType model_type() const noexcept = 0;

    /**
     * @brief Virtual initialisation hook — see the lifecycle in the class doc.
     *
     * Parse the endpoint out of the stored config, bind process-level
     * services (the event sink) to the plugin's SSE handler, warm caches.
     * Called by create_model before the model is handed out; never by the
     * host directly. Returning false must leave nothing needing release().
     */
    virtual bool build() noexcept { return true; }

    /**
     * @brief Virtual teardown hook — see the lifecycle in the class doc.
     *
     * Guaranteed to run exactly once, on the complete object, immediately
     * before destruction — never call it yourself.
     */
    virtual void release() noexcept {}

    /**
     * @brief Stages 1+2: run one whole conversation exchange.
     *
     * Builds the provider request from @p conversation plus the stored
     * config (input translation) and drives it to a finished response
     * (request & response) — one endpoint::complete pass, retries included.
     * Returns the assembled model_io::MessageItem: possibly a model response
     * carrying invokes (the host executes the tools and calls converse()
     * again after folding the results with integrate()), or the final
     * content-bearing response of the turn.
     *
     * NOT this method's job: executing tools, looping, state updates — the
     * caller owns the agent loop; converse() is one exchange of it.
     *
     * Cancellation: co_awaiting code may cancel the operation through the
     * executor; the coroutine should then complete without side effects
     * beyond what the provider already did.
     *
     * @throws LLMUnsupportedOperation  on the default body — this model does
     *         not implement conversation exchange.
     * @throws endpoint::HttpRequestException-family failures from the
     *         plugin's transport once the exchange is attempted (the exact
     *         type is provider-owned; it crosses the DSO boundary by
     *         std::exception).
     */
    virtual boost::asio::awaitable<model_io::MessageItem> converse(
        const model_io::AgentInputState& conversation) {
        (void)conversation;
        throw LLMUnsupportedOperation(
            "converse() is not implemented by this model");
    }

    /**
     * @brief Stage 3: fold one finished MessageItem into the conversation.
     *
     * The generic placement by the item's type (see below). Providers
     * override for retention policy — e.g. keeping reasoning_content alive
     * in replayed history (deepseek thinking mode requires it) where others
     * may strip it — NOT to change the placement itself. Each call appends;
     * deduplication and compaction are the host's policies.
     *
     *   UserInput      -> a new UserLoopStep (a new turn), oldest-first order.
     *   ModelResponse  -> a new AgentLoopStep appended to the CURRENT (last)
     *                     user turn's agent_loop_step; if no turn exists yet
     *                     (a resumed conversation) a host turn is created.
     *   InvokeReturn   -> appended to the CURRENT agent step's
     *                     invoke_returns; an orphan result (no step yet) gets
     *                     host structures rather than being dropped — the
     *                     data always lands somewhere serialisable.
     *
     * Persistence contract: after any sequence of integrate() calls the state
     * remains serialisable through model_io's existing to_json (per record /
     * per step); the model itself never performs storage.
     */
    virtual void integrate(model_io::AgentInputState& state,
                           const model_io::MessageItem& item) {
        using model_io::MessageItemType;
        switch (item.type) {
            case MessageItemType::UserInput: {
                state.turns.emplace_back().user_input = item;
                return;
            }
            case MessageItemType::ModelResponse: {
                if (state.turns.empty()) {
                    state.turns.emplace_back();
                }
                state.turns.back().agent_loop_step.emplace_back().model_response = item;
                return;
            }
            case MessageItemType::InvokeReturn: {
                if (state.turns.empty()) {
                    state.turns.emplace_back();
                }
                auto& turn = state.turns.back();
                if (turn.agent_loop_step.empty()) {
                    turn.agent_loop_step.emplace_back();
                }
                auto& step = turn.agent_loop_step.back();
                if (!step.invoke_returns) {
                    step.invoke_returns.emplace();
                }
                step.invoke_returns->push_back(item);
                return;
            }
        }
    }

    /**
     * @brief The provider's catalogue, live: one GET over the stored endpoint.
     *
     * Asks the provider what it currently offers — the OpenAI-compatible
     * "list models" query — and returns a JSON ARRAY, one object per offered
     * model, each the provider's own descriptor verbatim (typically "id",
     * "object", "owned_by", "created"; provider-specific fields ride along).
     * Richer per-model metadata (context window, pricing) appears in the
     * same entries when the provider supplies it.
     *
     * Like converse(), the coroutine runs on the stored executor and the
     * caller spawns. There is deliberately no retry: a catalogue query is
     * cheap to re-issue, and the retry engine is reader-shaped.
     *
     * @throws LLMUnsupportedOperation  on the default body — this model does
     *         not implement catalogue queries.
     * @throws endpoint::HttpRequestException-family failures once the query
     *         is attempted (connect, transport, non-200, or a payload that
     *         is neither an array nor the {"data": [...]} catalogue shape).
     */
    virtual boost::asio::awaitable<nlohmann::json> provider_info() {
        throw LLMUnsupportedOperation(
            "provider_info() is not implemented by this model");
    }

    /**
     * @brief Set generation knobs — typed tier: model + reasoning effort.
     *
     * The common path: the one pair hosts adjust most often, with a clear
     * type instead of JSON spelling. Translates into the same validated
     * merge as the JSON tier below — the "reasoning" envelope merges
     * recursively (sibling keys survive), an absent field leaves its knob
     * untouched. See set_generation(nlohmann::json) for the shared
     * contract (preconditions, rejected keys, atomicity).
     */
    virtual void set_generation(GenerationPreset preset) {
        nlohmann::json patch = nlohmann::json::object();
        if (preset.model) {
            patch["model"] = std::move(*preset.model);
        }
        if (preset.effort) {
            patch["reasoning"] = nlohmann::json{
                {"effort", std::string(to_string(*preset.effort))}};
        }
        apply_generation_patch(std::move(patch));
    }

    /**
     * @brief Set generation knobs — JSON tier: RFC 7386 merge-patch.
     *
     * Keys present in @p patch are written (nested objects merge
     * recursively), JSON null erases its key, keys absent from the patch
     * keep their current values — callers state only what changes:
     *
     *     model->set_generation(nlohmann::json{{"temperature", 0.9}});
     *     model->set_generation(nlohmann::json{{"reasoning", nullptr}});
     *
     * The patch must be an object. The three host-owned keys ("endpoint",
     * "provider", "retry") are configuration, not generation knobs, and
     * are rejected; so is a patch that would leave "model" missing or
     * empty. Interpreter-owned keys ("messages", "tools", "stream", ...)
     * may be set but are forced by the request builder at exchange time.
     *
     * _config is never touched: it stays the verbatim construction record
     * and these knobs are runtime overlays. Validation is atomic — a
     * rejected patch changes nothing. The idempotent second build() an
     * adapter allows does not reset the knobs (its _built guard returns
     * early), so patches survive.
     *
     * @throws std::logic_error        before build() populated the
     *                                generation object (or on a model type
     *                                that has none).
     * @throws std::invalid_argument   on a non-object patch, a host-owned
     *                                key, or a patch that would leave
     *                                "model" missing or empty.
     */
    virtual void set_generation(nlohmann::json patch) {
        apply_generation_patch(std::move(patch));
    }

    /// The generation knobs currently in effect: config minus the
    /// host-owned keys as build() derived it, plus whatever
    /// set_generation() has layered on. Read-only; mutation goes through
    /// the set_generation() tiers.
    const nlohmann::json& generation() const noexcept { return _generation; }

protected:
    /**
     * @brief Construct a model bound to one executor and one configuration.
     *
     * Protected: instances are minted by the plugin's exported
     * create_llm_model alias (or an in-memory test subclass) — construction
     * and configuration injection are one atomic step, so no half-built
     * model can escape. Everything the exchange needs lives in @p config
     * (see "The config JSON" in the file header); build() is where the
     * plugin parses it.
     */
    LLMModel(boost::asio::any_io_executor executor, nlohmann::json config)
        : _executor(std::move(executor)), _config(std::move(config)) {}

    /**
     * @brief The validated merge core both set_generation() tiers funnel
     *        through (and overrides may reuse after their own checks).
     *
     * Preconditions, the RFC 7386 merge, the "model" invariant, and the
     * atomic commit — exactly the contract on set_generation(nlohmann::json).
     */
    void apply_generation_patch(nlohmann::json patch) {
        if (!_generation.is_object()) {
            throw std::logic_error(
                "set_generation(): this model has no generation object "
                "(not built, or not a generation-knob model)");
        }
        if (!patch.is_object()) {
            throw std::invalid_argument(
                "set_generation(): the patch must be a JSON object");
        }
        for (const char* host_key : {"endpoint", "provider", "retry"}) {
            if (patch.contains(host_key)) {
                throw std::invalid_argument(
                    std::string("set_generation(): '") + host_key +
                    "' is host-owned configuration, not a generation knob");
            }
        }
        // nlohmann's merge_patch applies RFC 7386 in place and returns void,
        // so merge on a copy and commit only if every check below passes.
        nlohmann::json merged = _generation;
        merged.merge_patch(patch);
        const auto model = merged.find("model");
        if (model == merged.end() || !model->is_string() ||
            model->get_ref<const std::string&>().empty()) {
            throw std::invalid_argument(
                "set_generation(): the patch would leave \"model\" "
                "missing or empty");
        }
        _generation = std::move(merged);
    }

    /// Executor every exchange coroutine of this model runs on.
    boost::asio::any_io_executor _executor;
    /// The whole model configuration, verbatim as create_model received it.
    nlohmann::json _config;
    /// Generation knobs in effect: build()'s derivation of _config (minus
    /// the host-owned keys) plus the set_generation() overlays. Null until
    /// build() populates it, which is what makes set_generation() refuse
    /// pre-build use.
    nlohmann::json _generation;
};

/**
 * @brief Abstract descriptor + model factory for one provider.
 *
 * Specialises extension::ExtensionContext for the llm domain, mirroring
 * indextools::LangPlugin: the descriptor is long-lived and stateless beyond
 * the warm() factory cache; instances are produced by the plugin's exported
 * create_llm_plugin alias and owned by the host's LLMDispatcher.
 *
 * The routing key is name() — the provider name ("deepseek", "openai", ...).
 */
class LLMModelExtensionContext : public extension::ExtensionContext {
public:
    /**
     * @brief The model shape this provider's models implement. Routing /
     *        listing metadata only — NOT a gate (a provider may ship several
     *        shapes behind one signed factory and decide per config).
     */
    virtual LLMModelType get_type() const noexcept {
        return LLMModelType::Conversation;
    }

    /**
     * @brief Resolve and cache the model factory. Call once after the
     *        descriptor exists (at load or first use); idempotent and
     *        thread-safe — concurrent first calls (e.g. create_model racing
     *        on an add()-registered context) serialise here, so the cached
     *        factory and flags are always published before any mint().
     *
     * With a bound library, resolves the signed create_llm_model alias once
     * (the hot minting path is then a single indirect call). Without a
     * library — an in-memory context whose mint() override constructs models
     * directly — there is nothing to resolve and warm() succeeds.
     *
     * @return true if the descriptor can mint models (factory resolved, or
     *         the in-memory mint() path).
     */
    bool warm() noexcept {
        std::lock_guard<std::mutex> lock(_warm_mutex);
        if (_warmed) {
            return _valid;
        }
        _warmed = true;
        _valid = false;

        _library_ref = get_library_ref();
        if (_library_ref) {
            try {
                _factory = extension::detail::resolve_factory_alias<model_factory_signature>(
                    _library_ref, LLM_MODEL_FACTORY_NAME);
                _valid = true;
            } catch (const std::exception& e) {
                logging::Logger::warning(
                    std::string("llm plugin model factory resolution failed: ")
                    + e.what());
                _factory = nullptr;
            }
        } else {
            // In-memory context: mint() is overridden, nothing to resolve.
            _valid = true;
        }
        return _valid;
    }

    /**
     * @brief Mint one configured model, with the lifecycle guaranteed.
     *
     * The full discipline from the LLMModel class doc, so no caller can get
     * it wrong: mint (via the warmed factory, or the mint() override) ->
     * build() on the complete object -> a shared_ptr whose deleter captures
     * the plugin library, runs release(), and only then deletes. The model
     * is therefore safely destructible independently of this descriptor and
     * the dispatcher, and release() always reaches the most-derived override.
     *
     * @return the ready model, or nullptr if minting failed, build()
     *         returned false, or warm() never succeeded.
     */
    std::shared_ptr<LLMModel> create_model(
        boost::asio::any_io_executor executor,
        const nlohmann::json& config) const noexcept {
        std::unique_ptr<LLMModel> minted = mint(std::move(executor), config);
        if (minted == nullptr) {
            return nullptr;
        }
        // Virtual initialisation on the complete object (constructors are not
        // virtual). A false build() must have released its own resources —
        // plain destruction, no release() call.
        if (!minted->build()) {
            return nullptr;
        }
        // release()-then-delete, with the plugin library pinned until after
        // the deleting-destructor has returned (the deleter's captured
        // _library_ref is destroyed only after this body finishes). For an
        // in-memory context the captured handle is null and pins nothing.
        std::shared_ptr<LLMModel> model(minted.release(),
            [library = _library_ref](LLMModel* m) {
                m->release();
                delete m;
            });
        return model;
    }

protected:
    /// The signed factory the create_llm_model alias resolves to. Unlike the
    /// framework's nullary product_factory signature, it carries the
    /// (executor, config) injection the LLMModel constructor requires.
    using model_factory_signature =
        std::unique_ptr<LLMModel>(boost::asio::any_io_executor, const nlohmann::json&);

    /**
     * @brief Mint one un-built model instance. Override to construct
     *        in-memory models (tests, statically-linked providers) without a
     *        dynamic library; the default calls the warmed factory alias.
     *
     * The override must NOT call build() — create_model owns the lifecycle.
     */
    virtual std::unique_ptr<LLMModel> mint(
        boost::asio::any_io_executor executor,
        const nlohmann::json& config) const noexcept {
        if (!_valid || _factory == nullptr) {
            return nullptr;
        }
        try {
            return _factory(std::move(executor), config);
        } catch (...) {
            return nullptr;
        }
    }

private:
    /// Cached signed model factory (set by warm()); null until warmed.
    model_factory_signature* _factory = nullptr;
    /// Library the factory lives in (pins it for the descriptor's lifetime
    /// and is captured by every minted model's deleter).
    std::shared_ptr<boost::dll::shared_library> _library_ref;
    /// Serialises warm()'s lazy first run (create_model may race on an
    /// add()-registered context); the mutex hand-off publishes _factory.
    std::mutex _warm_mutex;
    /// Guards against re-warming.
    bool _warmed = false;
    /// Whether warm() produced a usable minting path.
    bool _valid = false;
};

/**
 * @brief Host-side loader + router for model plugins.
 *
 * Mirrors indextools::LangDispatcher on the generic ExtensionDispatcher:
 * load_models() imports a directory of plugin .so files (ABI-gated, warmed,
 * left inert on failure), and create_model() routes a provider name to its
 * descriptor and mints a configured model. The default plugin location is
 * layered one level below the language plugins: <exe_dir>/plugins/llm.
 *
 * After loading, the registry is immutable — create_model() is a pure
 * concurrent read (find + verify + mint).
 */
class LLMDispatcher : public extension::ExtensionDispatcher {
public:
    /**
     * @brief One plugin's admission gate: ABI match + llm type + warm().
     *
     * Also embedded in create_model(), so a context registered through add()
     * (bypassing load_models) cannot dodge it. Failures are logged and the
     * context stays inert (create_model returns nullptr for it).
     *
     * @return true if @p ctx is a usable llm plugin descriptor.
     */
    static bool verify_llm_context(const ContextPtr& ctx) noexcept {
        try {
            if (ctx == nullptr) {
                return false;
            }
            if (ctx->abi_version() != LLM_PLUGIN_ABI_VERSION) {
                logging::Logger::warning(std::string("llm plugin ")
                    + std::string(ctx->name())
                    + " rejected: ABI version mismatch (plugin "
                    + std::to_string(ctx->abi_version()) + ", host "
                    + std::to_string(LLM_PLUGIN_ABI_VERSION) + ")");
                return false;
            }
            auto llm_ctx = std::dynamic_pointer_cast<LLMModelExtensionContext>(ctx);
            if (llm_ctx == nullptr) {
                logging::Logger::warning(std::string("llm plugin ")
                    + std::string(ctx->name())
                    + " rejected: context is not an LLMModelExtensionContext");
                return false;
            }
            // warm() logs its own resolution failures.
            return llm_ctx->warm();
        } catch (...) {
            return false;
        }
    }

    /**
     * @brief Load, ABI-gate, and warm every model plugin in @p directory.
     *
     * Tolerant of a missing/unreadable directory (returns the current usable
     * count instead of throwing). Per-plugin load failures are logged and
     * dropped by the underlying generic loader; contexts failing the gate
     * stay registered but inert (create_model returns nullptr for them).
     * Call once, single-threaded, before any create_model() query.
     *
     * @return the number of usable (loaded, ABI-matched, warmed) plugins.
     */
    std::size_t load_models(const std::filesystem::path& directory) {
        std::error_code dc;
        if (!std::filesystem::is_directory(directory, dc)) {
            return _usable;
        }
        try {
            load_directory(directory, extension::is_likely_dynamic_library,
                           extension::same_tag_always{LLM_PLUGIN_FACTORY_NAME});
        } catch (const std::exception&) {
            return _usable;
        }
        for (const auto& ctx : contexts()) {
            if (verify_llm_context(ctx)) {
                ++_usable;
            }
        }
        return _usable;
    }

    /// Load from `<executable_dir>/plugins/llm` (the default install
    /// location — layered below the language plugins' own plugins/ dir).
    std::size_t load_default_models() {
        boost::system::error_code ec;
        auto exe = boost::dll::program_location(ec);
        if (ec) {
            return _usable;
        }
        // Round-trip through .string() so this works whether Boost.DLL was
        // built against std::filesystem or boost::filesystem.
        std::filesystem::path exe_path(exe.string());
        return load_models(exe_path.parent_path() / "plugins" / "llm");
    }

    /**
     * @brief Route a provider name to its plugin and mint a configured model.
     *
     * The minted model carries the full create_model lifecycle guarantee
     * (build() ran; release() will run before deletion).
     *
     * @param provider  The descriptor name() — "deepseek", "openai", ...
     * @param executor  Executor the model's exchanges run on.
     * @param config    The whole model configuration (see the file header).
     * @return a ready model, or nullptr if no usable plugin is registered
     *         under @p provider.
     */
    std::shared_ptr<LLMModel> create_model(
        std::string_view provider,
        boost::asio::any_io_executor executor,
        const nlohmann::json& config) const {
        auto ctx = find(provider);
        if (ctx == nullptr || !verify_llm_context(ctx)) {
            return nullptr;
        }
        auto llm_ctx = std::dynamic_pointer_cast<LLMModelExtensionContext>(ctx);
        return llm_ctx ? llm_ctx->create_model(std::move(executor), config) : nullptr;
    }

    /// Number of plugins that loaded, ABI-matched, and warmed (the routable set).
    std::size_t usable_count() const noexcept { return _usable; }

private:
    std::size_t _usable = 0;
};

} // namespace llm
