#pragma once

//
// model_request.hpp — model interpreter interfaces: request + response
// ====================================================================
//
// The two provider-neutral halves of one model invocation. The request half
// is the interface model_io.hpp reserves for the "interpreter": the
// component that turns one model invocation's inputs into a complete
// provider request. Three inputs, one product:
//
//   AgentInputState  (dataclass/model_io.hpp)         the conversation —
//                     system prompt, registered tools, turns
//   ModelEndpoint    (dataclass/endpoint_config.hpp)  the deployment —
//                     base_url, auth, transport headers
//   generation       (nlohmann::json)                 the per-call generation
//                     parameters (model, stream, temperature, ...) — plain
//                     JSON, deliberately not a struct: read-only caller
//                     configuration, not contract data that flows or is
//                     stored and loaded
//
// The product is exactly what this module's transport consumes — the request
// drivers (sse_request, HttpRequestDriver) take it by value, and the
// connection flows between them as one flavour-agnostic connection_stream
// value, so a built request plus a resolved endpoint plug straight into the
// existing pipeline:
//
//   interpreter->build_request(state, endpoint, generation)
//     -> resolve_endpoint(...) -> create_connection_stream(executor, resolved)
//        — one RUNTIME flavour choice (tls ? TLS : plain) behind the
//        move-only connection_stream facade
//     -> reader->pump(sse_request<Delta>, std::move(stream),
//                     std::move(request))
//        (or HttpRequestDriver<Delta> for a bounded, whole-body
//        exchange through the same pump)
//
// The response half is ModelResponseReader below: the single consumer that
// drains the SSE handler's get() side, folds each decoded delta into the
// provider's accumulators, offers every delta to observation hooks, and —
// on the terminal event — assembles the final model_io::MessageItem. Its
// pump() member drives the matching producer side (any RequestDriver plus
// the finish-on-exit the transport deliberately leaves to the caller, so a
// stream ending without the terminal event cannot strand the consumer).
// Where the interpreter maps contract data onto the wire, the reader maps
// the streamed wire back onto contract data; its dedicated SSE handler does
// the event decoding in between (decode only — accumulation and assembly
// live in the reader, so one handler stays a pure function of its events).
//
// Both interfaces are provider-neutral and this header-only module carries
// no concrete interpreter: implementations (with their dedicated SSE
// handlers and readers) are compiled libraries of their own — e.g. the
// Responses-API compatibility layer. Provider-agnostic building blocks that
// prove out across implementations (endpoint resolution today) live here.
//
// Implementation contract
// -----------------------
//  * Synchronous, pure, no I/O — building never opens a socket, so it is
//    fully offline-testable. Implementations should be stateless: one
//    instance may serve concurrent build_request() calls.
//  * Input leniency: an imperfect conversation must still build. Tolerate
//    empty turns (messages reduce to the system message, or none), empty
//    roles (derive them from MessageItemType), binary / external_ref content
//    (send `raw` as the string it is), absent invoke_returns (no
//    placeholders), and invokes whose names are not in the tools registry
//    (the registry is not a wire filter). Generation JSON passes through to
//    the request body verbatim except: "model" must be a non-empty string,
//    "stream" defaults to true when absent, and builder-owned keys
//    ("messages", "tools") win over same-named generation keys.
//  * Two hard errors only, reported as HttpRequestException with stage
//    CreateRequest (the module's existing request-lifecycle exception):
//      - generation carries no non-empty "model"
//      - base_url resolves to no host (see resolve_endpoint)
//  * Tool results correlate to their calls through the embedded provenance
//    record (MessageItem::invoke_return->query.id — the wire "tool call
//    id"); when it is absent, implementations may fall back to positional
//    alignment with the response's invokes, then to omitting the
//    correlation key.
//

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include "dataclass/endpoint_config.hpp"
#include "dataclass/model_io.hpp"
#include "endpoint/http_request_exception.hpp"
#include "endpoint/https_stream.hpp"
#include "endpoint/request.hpp"

#include "logging/logger.hpp"

namespace endpoint {

/**
 * @brief Turns one model invocation's inputs into a complete provider request.
 *
 * Pure interface — see the header contract above. Concrete interpreters are
 * compiled libraries (Responses-API layer first); this header-only module
 * defines only the contract they share.
 */
class ModelRequestInterpreter {
public:
    /// The request message this module's transport consumes; sse_request
    /// takes exactly this type by value.
    using HttpRequest = boost::beast::http::request<boost::beast::http::string_body>;

    virtual ~ModelRequestInterpreter() = default;

    /**
     * @brief Build one complete provider request.
     *
     * Synchronous, pure, offline. Lenient on imperfect inputs (contract
     * above); throws HttpRequestException{Stage::CreateRequest} only for a
     * missing model name or a hostless base_url.
     *
     * @param conversation The session's conversation state.
     * @param endpoint     Where and how to reach the provider.
     * @param generation   Generation parameters as plain JSON (owns "model",
     *                     "stream", "temperature", ...).
     */
    virtual HttpRequest build_request(
        const model_io::AgentInputState& conversation,
        const model_io::ModelEndpoint& endpoint,
        const nlohmann::json& generation) = 0;
};

/**
 * @brief Consumes one provider stream: deltas out, one ModelResponse in.
 *
 * The response half of this module's interpreter contract (see the header
 * block): the single consumer — the get side of SSEResponseHandler's SPSC
 * discipline — that drives the handler's channel, folds every decoded delta
 * into the subclass's accumulators, offers each delta to the observation
 * hooks, and on the terminal event assembles the final contract record (a
 * model_io::MessageItem shaped as one model response) before finishing the
 * handler. The dedicated SSE handler keeps only the event->delta decode.
 *
 * The hooks replace the former PeekingHandler with the same guarantee —
 * every delta exactly once, in arrival order, read-only — but they run on
 * the consumer side of the channel, inside next(), where a hook may safely
 * take its time: an async hook (an awaitable) is co_awaited inline, so slow
 * I/O back-pressures the stream through the channel instead of stalling the
 * network reads mid-decode. Type erasure, not templates: any callable
 * attaches to any reader at runtime — many hooks per reader, one functor
 * reused across readers — without a concrete type per (reader, hook)
 * combination.
 *
 * Canonical consume loop (the reader owns the handler lifecycle — the
 * consumer never calls finish() itself, and the producer side is pump(),
 * which finishes the handler on exit so a stream that ends without the
 * terminal event cannot leave next() blocked forever):
 *
 *     auto reader = std::make_shared<responses::ResponsesReader>(executor);
 *     reader->add_hook([](const responses::ResponsesDelta& d) {
 *         if (d.kind == responses::DeltaKind::ReasoningText)
 *             std::cerr << d.text << std::flush;   // watch it think
 *     });
 *     // producer, one co_spawn: pump() = driver + finish on exit.
 *     //   co_spawn(io, [reader, resolved]() {
 *     //       return reader->pump(
 *     //           endpoint::sse_request<responses::ResponsesDelta>,
 *     //           create_connection_stream(resolved),
 *     //           interpreter->build_request(...)); }, asio::detached);
 *     while (auto delta = co_await reader->next()) { ... live view ... }
 *     consume(reader->response());         // the assembled MessageItem
 *
 * Exception policy — centralized here, mirroring SSEResponseHandler::put:
 *   * A stream abort (SSEAborted out of get(): the channel was closed via
 *     finish() — the producer's error path, or abort() below) is a
 *     LIFECYCLE event, not an error: next() absorbs it and returns nullopt;
 *     the carried state survives via _aborted()/_abort_state() for the
 *     subclass's status(). Only get()'s abort is absorbed — an SSEAborted
 *     thrown by a HOOK is a hook fault and takes the fault path below.
 *   * A throwing hook or _accumulate override is a bug: next() drives the
 *     handler to ERROR (force-aborting the producer side too) and rethrows
 *     — the fault is never swallowed. On the terminal delta this happens
 *     AFTER assemble, so response()/status() stay coherent (final record,
 *     wire status) even when a monitor dies on the last event.
 *
 * Concurrency: this reader is the ONE consumer of its handler; at most one
 * next()/consume() may be in flight (they drive get()). add_hook()/
 * add_async_hook()/abort() are not synchronized with it — register hooks
 * before the stream starts, or externally serialized with next(). Hooks run
 * inside next() on the consumer side: they must not call back into the
 * reader (next/abort — reentrancy) nor touch the handler's put side.
 * response()/finished() read unsynchronized accumulators — call them only
 * once next() has returned nullopt (or from the producer side, after it has
 * exited).
 *
 * @tparam Delta  The handler's decoded event type (its product_type), which
 *                the subclass accumulates. One reader per response.
 */
template<typename Delta>
class ModelResponseReader {
public:
    /// The SSE handler whose get() side this reader drives.
    using Handler = SSEResponseHandler<Delta>;
    /// Type-erased sync observer: receives each delta by const reference.
    using Hook = std::function<void(const Delta&)>;
    /// Type-erased async observer: co_awaited inline per delta, may do I/O.
    using AsyncHook =
        std::function<boost::asio::awaitable<void>(const Delta&)>;

    virtual ~ModelResponseReader() = default;

    // One reader is one consumer over shared channel state; copying or
    // moving it would leave two, so both are deleted (as on the handler).
    ModelResponseReader(const ModelResponseReader&) = delete;
    ModelResponseReader& operator=(const ModelResponseReader&) = delete;
    ModelResponseReader(ModelResponseReader&&) = delete;
    ModelResponseReader& operator=(ModelResponseReader&&) = delete;

    /// The handler this reader drains — hand it to the producer side, e.g.
    /// sse_request(reader->handler(), std::move(stream), request).
    const std::shared_ptr<Handler>& handler() const noexcept {
        return _handler;
    }

    /**
     * @brief Register a synchronous read-only observer; callable more than
     *        once. Hooks (sync first, then async, each in registration
     *        order) run inside next() after _accumulate() has folded the
     *        delta — the same position the former peeker observed at.
     */
    void add_hook(Hook hook) { _hooks.push_back(std::move(hook)); }

    /// Register an async observer; same contract as add_hook(), but the
    /// awaitable is co_awaited inline: it may do I/O, and while it runs the
    /// stream back-pressures instead of racing ahead.
    void add_async_hook(AsyncHook hook) {
        _async_hooks.push_back(std::move(hook));
    }

    /**
     * @brief Await the next delta of the stream.
     *
     * Returns every delta in arrival order — including the terminal marker
     * itself — and nullopt once the stream is over: the terminal event was
     * seen (the reader then assembled the response and finished the handler
     * DONE), or the channel was closed (producer fault / abort(); absorbed
     * as a lifecycle end, see the exception policy in the class doc).
     * Idempotent: every call after the first nullopt returns nullopt.
     *
     * @throws whatever a hook or the _accumulate override throws — after the
     *         handler has been driven to ERROR, so the producer side tears
     *         down too.
     */
    boost::asio::awaitable<std::optional<Delta>> next() {
        if (_done) co_return std::nullopt;

        std::optional<Delta> delta;
        try {
            delta = co_await _handler->get();
        } catch (const SSEAborted& aborted) {
            // A closed channel is a stream END, not a consumer error: record
            // the carried state for status() and end quietly. Scoped to
            // get() ONLY — an SSEAborted thrown by a hook below is a hook
            // fault and takes the catch-all path like any other exception.
            _abort = aborted.state();
            _abort_reason = aborted.what();
            _done = true;
            co_return std::nullopt;
        }

        try {
            // Fold FIRST: the terminal marker's own payload (status, usage,
            // terminal details) must be in the accumulators before
            // _assemble() reads them.
            _accumulate(*delta);

            if (_is_terminal(*delta)) {
                // And assemble BEFORE the hooks run: once the wire's
                // terminal event is folded in, the record is final — a hook
                // fault on the terminal delta must not leave a "Completed"
                // status beside an unassembled response().
                _assemble();
                _handler->finish();   // DONE — the consumer owns no lifecycle
                _done = true;
            }

            for (const auto& hook : _hooks) hook(*delta);
            for (const auto& hook : _async_hooks) co_await hook(*delta);
        } catch (...) {
            // Hook or accumulation fault: same policy as put()'s catch(...)
            // — drive the handler to ERROR (unblocking/aborting the producer
            // side), mark the stream over, and surface the bug.
            _handler->finish(Handler::State::ERROR);
            _done = true;
            throw;
        }
        co_return delta;
    }

    /**
     * @brief Drain the stream to its end and return the assembled response.
     *
     * The next() loop in one call, for consumers that only want the final
     * MessageItem — hooks still observe every delta, which is the point:
     * live monitoring plus the contract record from one drain.
     */
    boost::asio::awaitable<model_io::MessageItem> consume() {
        while (co_await next()) {
        }
        co_return response();
    }

    /// The assembled contract record. Meaningful only after the stream
    /// ended (see the concurrency note in the class doc).
    virtual const model_io::MessageItem& response() const = 0;

    /// Whether the stream is over — the terminal event was seen or the
    /// channel closed; next() would return nullopt.
    bool finished() const noexcept { return _done; }

    /**
     * @brief End the stream early, from the consumer side.
     *
     * For a consumer that will not drain to the terminal event (a deadline,
     * a cancellation): marks the stream over and closes the channel via
     * finish(ERROR), so a blocked or future get() ends in SSEAborted —
     * absorbed as a lifecycle end (see _aborted()) — and the producer's
     * put() stops cooperatively. Serialize with next(), like every control
     * here. A delta already mid-handover may still be delivered by an
     * in-flight next(); the one after returns nullopt.
     */
    void abort() {
        _done = true;
        _handler->finish(Handler::State::ERROR);
    }

    /**
     * @brief Drive this reader's producer side to its end, with any request
     *        driver.
     *
     * The driver alone leaves the handler unfinished when it returns — by
     * its own contract, the caller decides the lifecycle. For the canonical
     * wiring that is a hang waiting to happen: when the stream ends WITHOUT
     * the terminal event (transport EOF or truncation, a non-200 rejection,
     * a proxy ending on `[DONE]`), a consumer blocked in next() would wait
     * on the never-closed channel forever. This wrapper closes the gap: it
     * runs the driver and, on ANY exit — clean server close, cooperative
     * stop, or a transport fault (finished first, then rethrown) — finishes
     * the handler when the reader has not already, so the consumer wakes
     * and classifies the end itself (status() reports Aborted unless the
     * terminal event had already completed the stream).
     *
     * This coroutine IS the handler's one producer (see the concurrency
     * note); the consumer side is next()/consume() elsewhere, on the same
     * shared reader.
     *
     * @tparam Driver   The request driver for THIS exchange, constrained by
     *                  RequestDriver: sse_request<Delta> passed by value
     *                  (it decays to a pointer) for streaming, or
     *                  HttpRequestDriver<Delta> for a bounded whole-body
     *                  exchange; deduced from the argument.
     * @param driver    The driver to run the exchange with.
     * @param stream    Single-use connection of either flavour (the
     *                  connection_stream facade); created by the caller
     *                  (create_connection_stream, directly from a
     *                  ResolvedEndpoint).
     * @param request   Fully constructed request (an interpreter's
     *                  build_request product).
     */
    template<RequestDriver<Handler> Driver>
    boost::asio::awaitable<void> pump(
        Driver driver,
        connection_stream stream,
        ModelRequestInterpreter::HttpRequest request) {
        try {
            co_await driver(_handler, std::move(stream), std::move(request));
        } catch (...) {
            // Transport fault: the stream can produce nothing more — abort
            // the consumer too (it would otherwise block forever), then
            // surface the failure.
            _handler->finish(Handler::State::ERROR);
            throw;
        }
        // Clean pump exit (server closed / cooperative stop). If the reader
        // has not already ended the stream (terminal seen -> it finished
        // the handler DONE), nothing further can arrive: finish now so a
        // consumer blocked in next() wakes. finish() repeats safely; the
        // RUNNING check just preserves a DONE the reader already recorded.
        if (_handler->get_state() == Handler::State::RUNNING) {
            _handler->finish(Handler::State::ERROR);
        }
    }

protected:
    /**
     * @brief Adopt the handler whose get() side this reader will drive.
     *
     * @throws std::invalid_argument when @p handler is null.
     */
    explicit ModelResponseReader(std::shared_ptr<Handler> handler)
        : _handler(std::move(handler)) {
        if (!_handler) {
            throw std::invalid_argument(
                "ModelResponseReader: null SSE handler");
        }
    }

    /// Fold one delta into the accumulators. Must not throw for a
    /// well-formed delta (decode faults never reach here — the handler's
    /// decode path is exception-total); a throw is a reader bug that kills
    /// the stream, cf. next()'s exception policy.
    virtual void _accumulate(const Delta& delta) = 0;

    /// Whether this delta is the provider's terminal lifecycle event —
    /// next()'s signal to assemble and finish. Called after _accumulate().
    virtual bool _is_terminal(const Delta& delta) const = 0;

    /// Build the final MessageItem into the subclass's response() storage.
    /// Called once, iff a terminal delta was seen; never on an aborted
    /// stream (there the accumulators simply die with the stream).
    virtual void _assemble() = 0;

    /// Whether the stream ended through a closed channel (producer fault or
    /// abort()) rather than a terminal event.
    bool _aborted() const noexcept { return _abort.has_value(); }

    /// The handler state carried by the SSEAborted that ended the stream
    /// (ERROR from the producer's fault path or abort(); DONE when some
    /// external finish() closed it cooperatively).
    SSEHandlerState _abort_state() const noexcept {
        return _abort.value_or(SSEHandlerState::ERROR);
    }

    /// The message of the SSEAborted that ended the stream, for logs.
    const std::string& _abort_message() const noexcept {
        return _abort_reason;
    }

    std::shared_ptr<Handler> _handler;   ///< shared with the producer side

private:
    bool _done = false;
    std::optional<SSEHandlerState> _abort;   // set iff ended via SSEAborted
    std::string _abort_reason;
    std::vector<Hook> _hooks;
    std::vector<AsyncHook> _async_hooks;
};

// ---- shared building block -------------------------------------------------

/// Where one request goes: the parsed view of a ModelEndpoint.
struct ResolvedEndpoint {
    std::string host;
    std::string port = "443";
    std::string target{};
    /// Whether the transport must use TLS — create_connection_stream(*this)
    /// turns this runtime flag into the matching flavour inside the returned
    /// connection_stream (verified TLS when true, plain TCP when false).
    bool tls = true;

    /// host[:port] for the request's Host header — RFC 9110 §7.2 requires the
    /// port when it is non-default for the scheme (443 https / 80 http);
    /// vhost-routing proxies match on the authority, so dropping a non-default
    /// port misroutes the request.
    std::string authority() const {
        const std::string_view default_port = tls ? "443" : "80";
        if (port == default_port) return host;
        return host + ":" + port;
    }
};

/**
 * @brief Leniently resolve a ModelEndpoint into host/port/target.
 *
 *   * scheme optional (https assumed); http:// allowed for local backends;
 *     the default port follows the scheme (443 / 80) unless an explicit
 *     :port overrides it, and the tls flag records the scheme so callers can
 *     pick the matching connection factory
 *   * any path prefix in base_url is kept and request_path is appended
 *   * trailing slashes in either part are tolerated
 *
 * The one strict requirement: a non-empty host.
 *
 * @throws HttpRequestException{Stage::CreateRequest} when base_url (after
 *         stripping scheme and prefix) yields no host.
 */
inline ResolvedEndpoint resolve_endpoint(const model_io::ModelEndpoint& endpoint) {
    std::string rest = endpoint.base_url;

    bool tls = true; // https assumed when no scheme is given
    if (rest.rfind("https://", 0) == 0) {
        rest.erase(0, 8);
    } else if (rest.rfind("http://", 0) == 0) {
        rest.erase(0, 7);
        tls = false;
    }

    ResolvedEndpoint resolved;
    resolved.tls = tls;
    resolved.port = tls ? "443" : "80";

    const auto slash = rest.find('/');
    // rfind: with a bracketed IPv6 authority ([::1]:8443) the LAST colon is
    // the port separator.
    const std::string authority =
        (slash == std::string::npos) ? rest : rest.substr(0, slash);
    const std::string prefix =
        (slash == std::string::npos) ? std::string{} : rest.substr(slash + 1);

    if (const auto colon = authority.rfind(':'); colon != std::string::npos) {
        resolved.host = authority.substr(0, colon);
        resolved.port = authority.substr(colon + 1);
    } else {
        resolved.host = authority;
    }

    if (resolved.host.empty()) {
        throw HttpRequestException(
            HttpRequestException::Stage::CreateRequest,
            "base_url resolves to no host: \"" + endpoint.base_url + "\"");
    }

    // Join prefix and request_path with exactly one slash; tolerate missing
    // or doubled slashes on either side.
    std::string path = endpoint.request_path.empty() ? "/" : endpoint.request_path;
    if (path.front() != '/') path = "/" + path;
    std::string clean_prefix = prefix;
    while (!clean_prefix.empty() && clean_prefix.front() == '/')
        clean_prefix.erase(clean_prefix.begin());
    while (!clean_prefix.empty() && clean_prefix.back() == '/')
        clean_prefix.pop_back();
    resolved.target = clean_prefix.empty() ? path : "/" + clean_prefix + path;

    return resolved;
}

/**
 * @brief Apply a ModelEndpoint's transport headers to a built request.
 *
 * Shared by concrete interpreters (proven out by the Responses-API layer):
 * standard headers first — the auth credential per AuthScheme, then
 * User-Agent — then extra_headers, so user-supplied headers win over the
 * standard ones. An empty api_key never produces a credential header, and
 * AuthScheme::None produces none by design (fail-closed scheme).
 */
inline void apply_transport_headers(
    ModelRequestInterpreter::HttpRequest& request,
    const model_io::ModelEndpoint& endpoint) {
    namespace http = boost::beast::http;

    switch (endpoint.auth.scheme) {
        case model_io::AuthScheme::Bearer:
            if (!endpoint.auth.api_key.empty()) {
                request.set(http::field::authorization,
                            "Bearer " + endpoint.auth.api_key);
            }
            break;
        case model_io::AuthScheme::CustomHeader:
            if (!endpoint.auth.api_key.empty() &&
                !endpoint.auth.header_name.empty()) {
                request.set(endpoint.auth.header_name, endpoint.auth.api_key);
            }
            break;
        case model_io::AuthScheme::None:
            break;
    }

    if (!endpoint.user_agent.empty()) {
        request.set(http::field::user_agent, endpoint.user_agent);
    }
    for (const auto& [name, value] : endpoint.extra_headers) {
        request.set(name, value);
    }
}

/**
 * @brief Establish the connection a ResolvedEndpoint asks for.
 *
 * The scheme-to-factory step of this module's canonical wiring (header
 * block): ResolvedEndpoint records its scheme as a runtime bool, while the
 * transport above (a RequestDriver, reader->pump) consumes one
 * flavour-agnostic connection_stream value. This factory makes the choice at
 * RUNTIME — verified TLS (SNI, certificate check, handshake) when tls, plain
 * TCP otherwise — and returns the connected stream behind the move-only
 * facade, so no flavour (and no std::visit) ever leaks into a caller's
 * types:
 *
 *     auto stream = co_await create_connection_stream(resolved);
 *     co_await reader->pump(driver, std::move(stream), request);
 *
 * @param executor Executor on which DNS and socket operations run.
 * @param resolved Where to connect: host/port/tls exactly as resolve_endpoint
 *                 parsed them from the ModelEndpoint's base_url.
 * @param context  TLS client context; TLS flavour only, global by default —
 *                 pass a custom context for a private CA.
 * @return A connected connection_stream of the flavour the resolved scheme
 *         selected.
 * @throws boost::system::system_error on DNS, TCP, timeout, certificate, or
 *         TLS-handshake failure, as applicable to the flavour.
 */
inline boost::asio::awaitable<connection_stream>
create_connection_stream(
    boost::asio::any_io_executor executor,
    const ResolvedEndpoint& resolved,
    ssl_context& context = get_global_ssl_context())
{
    if (resolved.tls) {
        co_return connection_stream{
            co_await detail::connect_flavour<https_stream>::connect(
                executor, resolved.host, resolved.port, context)};
    }
    co_return connection_stream{
        co_await detail::connect_flavour<http_stream>::connect(
            executor, resolved.host, resolved.port, context)};
}

/**
 * @brief Connect on the calling coroutine's executor.
 *
 * Convenience overload for callers already inside an Asio coroutine; see the
 * executor-taking overload for details.
 */
inline boost::asio::awaitable<connection_stream>
create_connection_stream(
    const ResolvedEndpoint& resolved,
    ssl_context& context = get_global_ssl_context())
{
    auto executor = co_await boost::asio::this_coro::executor;
    co_return co_await create_connection_stream(executor, resolved, context);
}

} // namespace endpoint
