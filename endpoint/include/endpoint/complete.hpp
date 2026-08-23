#pragma once

//
// complete.hpp — one model invocation, end to end: connect, exchange, assemble
// ====================================================================
//
// The whole-exchange convenience over the canonical wiring (see the
// model_request.hpp header block): complete_once connects to the resolved
// endpoint, runs ONE driver exchange as a spawned producer task, drains the
// reader's consumer side inline, and hands back the reader's assembled
// model_io::MessageItem.
//
// Failure handling splits by side:
//   * Connect failures (the system_error family out of
//     create_connection_stream) are rethrown to the caller as
//     HttpRequestException{Stage::Connect}, keeping the transport error code
//     and the endpoint context. Nothing was spawned yet, so no producer side
//     exists to tear down.
//   * The spawned producer never lets an exception escape: pump() has already
//     finished the handler ERROR-side before rethrowing — which unblocks the
//     consumer below — so the failure is rendered into the log only, and the
//     exchange's outcome reaches this coroutine through the consumer's end
//     state instead.
//

#include <exception>
#include <utility>

#include <boost/asio.hpp>

#include "endpoint/http_request_exception.hpp"
#include "endpoint/model_request.hpp"

#include "logging/logger.hpp"

namespace endpoint {

/**
 * @brief Run one complete exchange against @p model_endpoint and return the
 *        assembled response.
 *
 * Connect first (its failures propagate as HttpRequestException{Stage::Connect}
 * carrying the transport error code and the endpoint's target/host context),
 * then the canonical two-sided wiring: the producer — @p driver pumped over a
 * freshly connected connection_stream — runs as a co_spawned task whose
 * exceptions are LOG-ONLY (pump has already torn the channel down for the
 * consumer by the time they are caught; see request.hpp's sse_request for the
 * exception contract), while this coroutine drains @p response_reader->consume()
 * inline and returns the assembled record. On a stream that ends without the
 * terminal event (a logged transport fault, a cooperative stop, a server that
 * closed early) the returned MessageItem is the reader's unassembled default —
 * the reader's end state, not this function, distinguishes those outcomes.
 *
 * @tparam Delta  The reader's decoded event type.
 * @tparam Driver The request driver for THIS exchange (sse_request<Delta> or
 *                HttpRequestDriver<Delta>), deduced from the argument.
 * @param executor         Executor both sides run on: the connect here and the
 *                         spawned producer task.
 * @param model_endpoint   Where to connect; resolved as resolve_endpoint
 *                         parsed it from the ModelEndpoint's base_url.
 * @param request          The fully built provider request (an interpreter's
 *                         build_request product).
 * @param response_reader  The consumer half; also drives accumulation and
 *                         assembly on its consume() side.
 * @param driver           The one-exchange producer driver.
 * @return The reader's assembled model_io::MessageItem.
 * @throws HttpRequestException{Stage::Connect} when the connection cannot be
 *         established; whatever consume() surfaces for consumer-side faults
 *         (a throwing reader hook).
 */
template<
    typename Delta,
    RequestDriver<typename ModelResponseReader<Delta>::Handler> Driver
>
boost::asio::awaitable<model_io::MessageItem> complete_once(
    boost::asio::any_io_executor executor,
    ResolvedEndpoint model_endpoint,
    ModelRequestInterpreter::HttpRequest request,
    std::shared_ptr<ModelResponseReader<Delta>> response_reader,
    Driver driver
) {
    // ---- connect -----------------------------------------------------------
    // create_connection_stream throws boost::system::system_error exclusively
    // (DNS, TCP, timeout, certificate, SNI — model_request.hpp documents the
    // family). The system_error branch runs for all of those and keeps
    // e.code(): the category (netdb / asio.system / ssl / beast timeout) is
    // what callers classify connect failures by, and to_string() renders its
    // message. The wider branches are defensive depth only.
    connection_stream stream{};
    try {
        stream = co_await create_connection_stream(executor, model_endpoint);
    } catch (const boost::system::system_error& e) {
        throw HttpRequestException(
            HttpRequestException::Stage::Connect,
            e.what(), e.code(), {},
            model_endpoint.target,
            model_endpoint.host
        );
    } catch (const std::exception& e) {
        throw HttpRequestException(
            HttpRequestException::Stage::Connect,
            e.what(), {}, {},
            model_endpoint.target,
            model_endpoint.host
        );
    } catch (...) {
        throw HttpRequestException(
            HttpRequestException::Stage::Connect,
            "unknown error", {}, {},
            model_endpoint.target,
            model_endpoint.host
        );
    }

    // ---- producer: one spawned exchange, log-only failures ------------------
    // The canonical co_spawn wiring: pump() drives the driver over the stream
    // and finishes the handler on every exit path, so by the time an exception
    // reaches these catch blocks the consumer's get() below is already
    // unblocked and the outcome is routed through the consumer side. Letting
    // it escape a detached spawn would only terminate the process — so every
    // failure is rendered (with request context, via wrap_request_failure for
    // the non-http strays) into the log and nothing else.
    boost::asio::co_spawn(
        executor,
        [
            driver,
            reader_ = response_reader,
            stream_ = std::move(stream),
            request_ = std::move(request)
        ] () mutable -> boost::asio::awaitable<void> {
            try {
                // request_ passes as an lvalue, so pump() takes its own copy
                // and request_ stays readable in the fallback catches below.
                co_await reader_->pump(driver, std::move(stream_), request_);
            } catch (const HttpRequestException& error) {
                // Already stage- and context-rich: render as-is.
                logging::Logger::error(error.to_string());
            } catch (const std::exception& error) {
                // Defensive depth (the drivers wrap their own failures into
                // HttpRequestException): fold the stray into the module's
                // lifecycle exception so the log line keeps the request
                // context. Stage::Unknown — nothing more precise is known.
                auto http_exception = wrap_request_failure(
                    HttpRequestException::Stage::Unknown,
                    error.what(),
                    {},
                    request_
                );
                logging::Logger::error(http_exception.to_string());
            } catch (...) {
                auto http_exception = wrap_request_failure(
                    HttpRequestException::Stage::Unknown,
                    "unknown error",
                    {},
                    request_
                );
                logging::Logger::error(http_exception.to_string());
            }
        },
        boost::asio::detached
    );

    // ---- consumer: inline drain to the assembled item ------------------------
    // The lambda's reader_ capture shares ownership, so the spawned producer
    // cannot outlive the reader even if it is still tearing down when this
    // coroutine finishes first.
    auto model_response = co_await response_reader->consume();
    co_return model_response;
}

} // namespace endpoint
