#pragma once

/**
 * @file llm/provider_models.hpp
 * @brief The provider-info queries shared by the protocol adapters.
 *
 * LLMModel::provider_info()'s implementation: one transport core
 * (fetch_provider_json — a single-shot GET that returns whatever JSON the
 * provider answered) plus the catalogue normaliser built on it
 * (fetch_provider_models — the OpenAI-compatible "list models" shape
 * reduced to the JSON array the contract promises). Header-only so both
 * shared adapters (llm_chat_completions / llm_responses) compile the same
 * coroutines without a new link dependency — under the
 * same-execution-context strategy that is the established pattern for
 * contract-adjacent code.
 *
 * Deliberately single-shot, no retry: the endpoint::complete engine is
 * reader-shaped for streaming exchanges, these queries are cheap to
 * re-issue, and their failures surface to the caller unchanged.
 */

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include "endpoint/model_request.hpp"
#include "endpoint/request.hpp"

namespace llm {

/**
 * @brief One provider-info GET, parsed: the transport core both the models
 *        catalogue and the balance companion ride on.
 *
 * GET <base_url prefix + @p request_path> with the endpoint's transport
 * headers (auth included) and co_return whatever JSON the provider
 * answered — no shape assumptions, the caller interprets.
 *
 * @param executor  Executor the connect and exchange run on.
 * @param endpoint  The model's stored endpoint (base_url/auth/headers);
 *                  its request_path is ignored in favour of @p request_path.
 *                  Caller keeps the endpoint (its model) alive across the
 *                  co_await — a reference parameter, not a frame copy.
 * @param request_path  The query's path ("/v1/models", "/user/balance", ...).
 *                  BY VALUE on purpose: this is a coroutine, and a
 *                  string_view parameter would dangle across the first
 *                  suspension when the caller passes a dialect hook's
 *                  temporary.
 * @param read_timeout_sec  Response read deadline; 0 waits indefinitely.
 * @throws endpoint::HttpRequestException — Stage::Connect when the
 *         connection cannot be established, Stage::HandleResponse for a
 *         non-200 reply (body folded into the message) or a body that is
 *         not JSON. Stage::CreateRequest propagates from resolve_endpoint
 *         for a hostless base_url. what() carries the request line
 *         (method + target + host), which is what tells the queries apart.
 */
inline boost::asio::awaitable<nlohmann::json> fetch_provider_json(
    boost::asio::any_io_executor executor,
    const model_io::ModelEndpoint& endpoint,
    std::string request_path,
    std::size_t read_timeout_sec = endpoint::DEFAULT_HTTP_READ_TIMEOUT_SEC)
{
    namespace http = boost::beast::http;
    // HttpRequestException is at global scope — endpoint's exception
    // contract predates the namespace (the "endpoint::" qualification in
    // prose around the tree is informal).

    // Route the query through the same prefix-join logic as the exchange
    // path: copy the endpoint, swap the request path, resolve.
    model_io::ModelEndpoint query_endpoint = endpoint;
    query_endpoint.request_path = std::move(request_path);
    endpoint::ResolvedEndpoint resolved =
        endpoint::resolve_endpoint(query_endpoint);

    // create_connection_stream throws boost::system::system_error
    // exclusively; fold into the module's lifecycle exception with the
    // endpoint context, exactly as complete_once does for exchanges.
    endpoint::connection_stream stream{};
    try {
        stream = co_await endpoint::create_connection_stream(executor, resolved);
    } catch (const boost::system::system_error& e) {
        throw HttpRequestException(
            HttpRequestException::Stage::Connect, e.what(), e.code(), {},
            resolved.target, resolved.host);
    } catch (const std::exception& e) {
        throw HttpRequestException(
            HttpRequestException::Stage::Connect, e.what(), {}, {},
            resolved.target, resolved.host);
    } catch (...) {
        throw HttpRequestException(
            HttpRequestException::Stage::Connect, "unknown error", {}, {},
            resolved.target, resolved.host);
    }

    http::request<http::string_body> request{
        http::verb::get, resolved.target, 11};
    request.set(http::field::host, resolved.authority());
    request.set(http::field::accept, "application/json");
    endpoint::apply_transport_headers(request, endpoint);

    // The direct bounded overload: no status gating, whole response back —
    // the caller wants to render non-200 bodies itself, like here.
    http::response<http::string_body> response = co_await endpoint::http_request(
        std::move(stream), std::move(request), read_timeout_sec);

    if (response.result() != http::status::ok) {
        constexpr std::size_t kMaxErrorBody = 2048;
        std::string body = response.body();
        if (body.size() > kMaxErrorBody) body.resize(kMaxErrorBody);
        std::string message = "provider info request rejected";
        if (!body.empty()) message += ": " + body;
        throw HttpRequestException(
            HttpRequestException::Stage::HandleResponse, std::move(message),
            {}, "GET", resolved.target, resolved.host, response.result_int());
    }

    try {
        co_return nlohmann::json::parse(response.body());
    } catch (const std::exception& e) {
        throw HttpRequestException(
            HttpRequestException::Stage::HandleResponse,
            std::string("provider info body is not JSON: ") + e.what(),
            {}, "GET", resolved.target, resolved.host);
    }
}

/**
 * @brief Fetch the provider's model catalogue as a JSON array.
 *
 * fetch_provider_json for the dialect's models_path, then normalise: a
 * top-level array passes through verbatim, the OpenAI-compatible
 * {"object": "list", "data": [...]} envelope unwraps to its "data" array.
 * Every entry is the provider's own model descriptor, untouched.
 *
 * @param executor  Executor the connect and exchange run on.
 * @param endpoint  The model's stored endpoint; see fetch_provider_json.
 * @param models_path  The dialect's catalogue path ("/v1/models", ...).
 * @param read_timeout_sec  Response read deadline; 0 waits indefinitely.
 * @throws endpoint::HttpRequestException — everything fetch_provider_json
 *         raises, plus Stage::HandleResponse for a payload that is neither
 *         an array nor the {"data": [...]} catalogue shape.
 */
inline boost::asio::awaitable<nlohmann::json> fetch_provider_models(
    boost::asio::any_io_executor executor,
    const model_io::ModelEndpoint& endpoint,
    std::string models_path,
    std::size_t read_timeout_sec = endpoint::DEFAULT_HTTP_READ_TIMEOUT_SEC)
{
    nlohmann::json payload = co_await fetch_provider_json(
        executor, endpoint, std::move(models_path), read_timeout_sec);

    if (payload.is_array()) {
        co_return payload;
    }
    if (payload.is_object() && payload.contains("data") &&
        payload["data"].is_array()) {
        // The OpenAI-compatible "list models" envelope.
        co_return payload["data"];
    }
    // No request line in context here (the transport core owns the
    // resolution); the message plus the provider_info() call site carry it.
    throw HttpRequestException(
        HttpRequestException::Stage::HandleResponse,
        "unexpected models catalogue payload (neither an array nor the "
        "{\"data\": [...]} shape)");
}

} // namespace llm
