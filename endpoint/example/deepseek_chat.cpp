// Interactive DeepSeek chat client built on the endpoint module.
//
// This is an integration example / smoke test against a real model backend. At
// startup it prompts for the DeepSeek-compatible base_url and an API key, then
// runs an interactive chat loop. Each turn POSTs the OpenAI-style
// /chat/completions request with stream=true and prints the streamed reply as it
// arrives, using the same machinery the rest of the module relies on:
//
//   * a connection_stream (a verified TLS connection via the global context,
//     chosen at runtime from the base_url's scheme)
//   * a RequestDriver (endpoint::sse_request here), injected into chat_turn
//     as a type parameter
//   * endpoint::SSEResponseHandler, specialised here to decode SSE deltas
//
// The DeepSeek streaming format is OpenAI-compatible: each event is one
// `data: <json>` line followed by a blank line, and the stream ends with
// `data: [DONE]`. Each delta JSON is roughly:
//   {"choices":[{"index":0,"delta":{"content":"Hello","reasoning_content":"..."}}]}
//
// Build target only; not registered with CTest (it needs a live API key).
#include "endpoint/http_request_exception.hpp"
#include "endpoint/model_request.hpp"
#include "endpoint/request.hpp"

#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <iostream>
#include <memory>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace asio = boost::asio;
namespace http = boost::beast::http;
// operator&& / operator|| for awaitables live in this nested namespace.
using namespace asio::experimental::awaitable_operators;

using endpoint::SSEAborted;
using endpoint::SSEHandlerState;
using endpoint::SSEResponseHandler;

// One streamed piece of an assistant message. `done` marks the terminal
// `data: [DONE]` sentinel.
struct Delta {
    std::string content;
    std::string reasoning;
    bool done = false;
};

// Return obj[key] only when it is a real JSON string; tolerate null/missing
// (DeepSeek emits "content": null on the role/usage deltas).
static std::string string_or_empty(const nlohmann::json& obj, const char* key) {
    if (obj.contains(key) && obj[key].is_string()) return obj[key].get<std::string>();
    return {};
}

// Frames the raw SSE byte stream (driven by the driver's put side) and decodes
// each OpenAI-style event into a Delta for the consumer to print.
class DeepSeekDeltaHandler final : public SSEResponseHandler<Delta> {
public:
    using SSEResponseHandler<Delta>::SSEResponseHandler;

    Delta _handle_message(std::span<const LineInfo> message) override {
        for (const auto& [field, value] : message) {
            if (field != "data") continue;   // ignore event:/id:/comments/etc.

            if (value == "[DONE]") return {.done = true};

            // Keep-alive or malformed payloads are ignored, not fatal.
            nlohmann::json parsed;
            try {
                parsed = nlohmann::json::parse(value);
            } catch (const nlohmann::json::parse_error&) {
                return {};
            }

            // Deltas often carry `"content": null` (e.g. the opening role delta
            // or the final usage chunk). json::value() only substitutes a
            // default when the key is *absent*, so pull strings defensively.
            Delta delta;
            const auto choices = parsed.value("choices", nlohmann::json::array());
            if (choices.is_array() && !choices.empty()) {
                const auto& choice = choices[0];
                if (choice.is_object() && choice.contains("delta") &&
                    choice["delta"].is_object()) {
                    const auto& d = choice["delta"];
                    delta.content = string_or_empty(d, "content");
                    delta.reasoning = string_or_empty(d, "reasoning_content");
                }
            }
            return delta;
        }
        return {};
    }
};

// A chat message: (role, content), serialised straight into the messages array.
using Message = std::pair<std::string, std::string>;

// Parsed view of the user-supplied base_url: how to reach /chat/completions.
struct ServiceEndpoint {
    std::string host;
    std::string port = "443";
    std::string target = "/chat/completions";
    bool tls = true;
};

// Split "https://host[:port][/prefix]" (or the http:// form) into host/port
// and the /chat/completions target. The OpenAI base_url has no prefix; the
// Anthropic one carries /anthropic. The scheme decides the transport flavour.
ServiceEndpoint parse_base_url(std::string base_url) {
    const std::string scheme = "https://";
    ServiceEndpoint endpoint;
    if (base_url.rfind(scheme, 0) == 0) {
        base_url.erase(0, scheme.size());
    } else if (base_url.rfind("http://", 0) == 0) {
        base_url.erase(0, std::string("http://").size());
        endpoint.tls = false;
        endpoint.port = "80";
    }

    const auto slash = base_url.find('/');
    std::string authority = (slash == std::string::npos) ? base_url : base_url.substr(0, slash);
    std::string prefix = (slash == std::string::npos) ? std::string{} : base_url.substr(slash);

    if (const auto colon = authority.find(':'); colon != std::string::npos) {
        endpoint.host = authority.substr(0, colon);
        endpoint.port = authority.substr(colon + 1);
    } else {
        endpoint.host = authority;
    }

    if (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
    endpoint.target = prefix + "/chat/completions";
    return endpoint;
}

// Default model. Override at runtime with the DEEPSEEK_MODEL environment variable.
static const char* model_name() {
    if (const char* env = std::getenv("DEEPSEEK_MODEL")) return env;
    return "deepseek-v4-flash";
}

// Build the POST request for one chat turn over the already-resolved history.
static http::request<http::string_body> build_request(
    const ServiceEndpoint& endpoint, const std::string& api_key,
    const std::vector<Message>& history) {
    nlohmann::json messages = nlohmann::json::array();
    for (const auto& [role, content] : history)
        messages.push_back({{"role", role}, {"content", content}});

    nlohmann::json body;
    body["model"] = model_name();
    body["stream"] = true;
    body["messages"] = messages;

    http::request<http::string_body> request{http::verb::post, endpoint.target, 11};
    // RFC 9110 §7.2: carry the port in Host when non-default (443 https/80 http).
    const bool default_port = endpoint.port == "443" || endpoint.port == "80";
    request.set(http::field::host,
                default_port ? endpoint.host
                             : endpoint.host + ":" + endpoint.port);
    request.set(http::field::authorization, "Bearer " + api_key);
    request.set(http::field::accept, "text/event-stream");
    request.set(http::field::content_type, "application/json");
    request.set(http::field::user_agent, "simplex-cpp-deepseek-example");
    request.body() = body.dump();
    request.prepare_payload();
    return request;
}

// One chat turn: connect, stream the response, return the full assistant reply.
//
// The request driver is a type parameter constrained by RequestDriver, so the
// turn machinery is agnostic to the exchange flavour; this example drives it
// with the SSE driver. The producer (the driver) and the consumer (the get()
// loop) must run concurrently because the underlying channel is rendezvous.
// They are launched together via make_parallel_group and both are awaited, so
// the captured locals stay alive for the whole turn.
template<endpoint::RequestDriver<DeepSeekDeltaHandler> Driver>
static asio::awaitable<std::string> chat_turn(
    Driver driver,
    const ServiceEndpoint& endpoint,
    const std::string& api_key,
    const std::vector<Message>& history) {
    auto executor = co_await asio::this_coro::executor;
    auto handler = std::make_shared<DeepSeekDeltaHandler>(executor);

    std::string reply;
    std::string error_text;

    auto producer = [&]() -> asio::awaitable<void> {
        try {
            auto stream = co_await endpoint::create_connection_stream(
                endpoint::ResolvedEndpoint{
                    .host = endpoint.host,
                    .port = endpoint.port,
                    .tls = endpoint.tls});
            auto request = build_request(endpoint, api_key, history);
            co_await driver(handler, std::move(stream), std::move(request));
        } catch (const HttpRequestException& error) {
            error_text = error.to_string();
        }
        handler->finish(SSEHandlerState::DONE);
    };

    auto consumer = [&]() -> asio::awaitable<void> {
        try {
            for (;;) {
                Delta delta = co_await handler->get();
                if (delta.done) break;
                if (!delta.reasoning.empty())
                    std::cerr << delta.reasoning << std::flush;
                if (!delta.content.empty()) {
                    reply += delta.content;
                    std::cout << delta.content << std::flush;
                }
            }
        } catch (const SSEAborted&) {
            // finish() closed the channel — normal end of the turn.
        }
    };

    // operator&& (wait_for_all) runs the producer and consumer concurrently and
    // resumes only once both have finished, so the captured locals (reply,
    // error_text) are safe for the whole turn.
    co_await (producer() && consumer());

    if (!error_text.empty()) std::cerr << "\n[" << error_text << "]\n";
    co_return reply;
}

int main() {
    std::cout << "=== DeepSeek chat-completion example (streaming) ===\n";

    std::string base_url;
    std::cout << "base_url [https://api.deepseek.com]: ";
    if (!std::getline(std::cin, base_url) || base_url.empty())
        base_url = "https://api.deepseek.com";

    std::string api_key;
    std::cout << "api_key: ";
    if (!std::getline(std::cin, api_key) || api_key.empty()) {
        std::cerr << "no api_key provided; exiting.\n";
        return 1;
    }

    const auto endpoint = parse_base_url(base_url);
    std::cout << "using " << endpoint.host << ":" << endpoint.port
              << endpoint.target << " (model: " << model_name() << ")\n";
    std::cout << "reasoning_content (if any) is streamed to stderr.\n";
    std::cout << "empty line to quit.\n";

    std::vector<Message> history{{"system", "You are a helpful assistant."}};

    std::string line;
    while (std::cout << "\nyou> " && std::getline(std::cin, line)) {
        if (line.empty()) break;
        history.emplace_back("user", line);

        std::string reply;
        try {
            asio::io_context io;
            auto future = asio::co_spawn(
                io,
                chat_turn(&endpoint::sse_request<Delta>,
                          endpoint, api_key, history),
                asio::use_future);
            io.run();
            reply = future.get();
        } catch (const std::exception& error) {
            std::cerr << "turn failed: " << error.what() << "\n";
            history.pop_back();   // drop the unanswered user message
            continue;
        }

        if (reply.empty())
            std::cout << "(no content received)";
        std::cout << "\n";

        history.emplace_back("assistant", reply);
    }

    return 0;
}
