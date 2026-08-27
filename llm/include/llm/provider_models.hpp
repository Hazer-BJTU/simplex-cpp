#pragma once

/**
 * @file llm/provider_models.hpp
 * @brief The provider catalogue query shared by the protocol adapters.
 *
 * LLMModel::provider_info()'s implementation: one OpenAI-compatible "list
 * models" GET over the model's stored endpoint, normalised to the JSON
 * array the contract promises. Header-only so both shared adapters
 * (llm_chat_completions / llm_responses) compile the same coroutine
 * without a new link dependency — under the same-execution-context
 * strategy that is the established pattern for contract-adjacent code.
 *
 * Deliberately single-shot, no retry: the endpoint::complete engine is
 * reader-shaped for streaming exchanges, a catalogue query is cheap to
 * re-issue, and its failures surface to the caller unchanged.
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
 * @brief Fetch the provider's model catalogue as a JSON array.
 *
 * GET <base_url prefix + @p models_path> with the endpoint's transport
 * headers (auth included), then normalise: a top-level array passes
 * through verbatim, the OpenAI-compatible {"object": "list", "data":
 * [...]} envelope unwraps to its "data" array. Every entry is the
 * provider's own model descriptor, untouched.
 *
 * @param executor  Executor the connect and exchange run on.
 * @param endpoint  The model's stored endpoint (base_url/auth/headers);
 *                  its request_path is ignored in favour of @p models_path.
 *                  Caller keeps the endpoint (its model) alive across the
 *                  co_await — a reference parameter, not a frame copy.
 * @param models_path  The dialect's catalogue path ("/v1/models", ...).
 *                  BY VALUE on purpose: this is a coroutine, and a
 *                  string_view parameter would dangle across the first
 *                  suspension when the caller passes
 *                  dialect->models_path()'s temporary.
 * @param read_timeout_sec  Response read deadline; 0 waits indefinitely.
 * @throws endpoint::HttpRequestException — Stage::Connect when the
 *         connection cannot be established, Stage::HandleResponse for a
 *         non-200 reply (body folded into the message), a body that is
 *         not JSON, or a payload that is neither array nor the {"data":
 *         [...]} shape. Stage::CreateRequest propagates from
 *         resolve_endpoint for a hostless base_url.
 */
inline boost::asio::awaitable<nlohmann::json> fetch_provider_models(
    boost::asio::any_io_executor executor,
    const model_io::ModelEndpoint& endpoint,
    std::string models_path,
    std::size_t read_timeout_sec = endpoint::DEFAULT_HTTP_READ_TIMEOUT_SEC)
{
    namespace http = boost::beast::http;
    // HttpRequestException is at global scope — endpoint's exception
    // contract predates the namespace (the "endpoint::" qualification in
    // prose around the tree is informal).

    // Route the catalogue through the same prefix-join logic as the
    // exchange path: copy the endpoint, swap the request path, resolve.
    model_io::ModelEndpoint catalogue_endpoint = endpoint;
    catalogue_endpoint.request_path = std::move(models_path);
    endpoint::ResolvedEndpoint resolved =
        endpoint::resolve_endpoint(catalogue_endpoint);

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
        std::string message = "models catalogue request rejected";
        if (!body.empty()) message += ": " + body;
        throw HttpRequestException(
            HttpRequestException::Stage::HandleResponse, std::move(message),
            {}, "GET", resolved.target, resolved.host, response.result_int());
    }

    nlohmann::json payload;
    try {
        payload = nlohmann::json::parse(response.body());
    } catch (const std::exception& e) {
        throw HttpRequestException(
            HttpRequestException::Stage::HandleResponse,
            std::string("models catalogue body is not JSON: ") + e.what(),
            {}, "GET", resolved.target, resolved.host);
    }

    if (payload.is_array()) {
        co_return payload;
    }
    if (payload.is_object() && payload.contains("data") &&
        payload["data"].is_array()) {
        // The OpenAI-compatible "list models" envelope.
        co_return payload["data"];
    }
    throw HttpRequestException(
        HttpRequestException::Stage::HandleResponse,
        "unexpected models catalogue payload (neither an array nor the "
        "{\"data\": [...]} shape)",
        {}, "GET", resolved.target, resolved.host);
}

} // namespace llm
