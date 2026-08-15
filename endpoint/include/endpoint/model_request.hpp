#pragma once

//
// model_request.hpp — model request interpreter interface
// =======================================================
//
// The interface model_io.hpp reserves for the "interpreter": the component
// that turns one model invocation's inputs into a complete provider request.
// Three inputs, one product:
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
// The product is exactly what this module's transport consumes:
// endpoint::sse_request(handler, stream, request) takes it by value, so a
// built request plugs straight into the existing pipeline:
//
//   interpreter->build_request(state, endpoint, generation)
//     -> create_https_connection_stream(host, port) -> sse_request(...)
//
// This interface is provider-neutral and this header-only module carries no
// concrete interpreter: implementations (with their dedicated SSE handlers)
// are compiled libraries of their own — e.g. the upcoming Responses-API
// compatibility layer. Provider-agnostic building blocks that prove out
// across implementations (endpoint resolution today) live here.
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

#include <string>

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include "dataclass/endpoint_config.hpp"
#include "dataclass/model_io.hpp"
#include "endpoint/http_request_exception.hpp"

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

// ---- shared building block -------------------------------------------------

/// Where one request goes: the parsed view of a ModelEndpoint.
struct ResolvedEndpoint {
    std::string host;
    std::string port = "443";
    std::string target;
};

/**
 * @brief Leniently resolve a ModelEndpoint into host/port/target.
 *
 *   * scheme optional (https assumed); http:// allowed for local backends;
 *     the default port follows the scheme (443 / 80) unless an explicit
 *     :port overrides it
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

} // namespace endpoint
