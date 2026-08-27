#pragma once

//
// endpoint_config.hpp — provider endpoint configuration
// =====================================================
//
// Where and how to reach one model provider: the deployment half of a model
// request. AgentInputState (model_io.hpp) is the conversation half, and its
// Scope note says model/endpoint settings (base_url, auth, ...) deliberately
// live outside the conversation — this header is where that half lives. An
// interpreter merges the two, plus the caller's generation settings, into one
// provider request. This type changes when the deployment changes, not when
// the conversation does.
//
// Generation settings (temperature, stream, ...) are intentionally NOT here
// either: they are caller-owned runtime parameters — adjustable at runtime
// through llm::LLMModel::set_generation(), not contract data that flows or
// is stored/loaded, so callers pass them as a plain nlohmann::json.
//
// Same contract rules as model_io.hpp: plain aggregates serialised through
// nlohmann ADL (a to_json/from_json pair next to each struct), snake_case
// keys, optionals omitted when empty and never null, enums as lowercase
// snake_case strings with the fail-safe value listed first (unknown values
// fall back to it on read), unknown keys ignored, find() guards instead of
// at(), round-trip invariant json(x).get<X>() reproduces x.
//

#include <map>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace model_io {

// ---- authentication ------------------------------------------------------

// How requests to the provider authenticate. None is listed first so an
// unrecognised scheme string on read fails closed: no credential is sent.
enum class AuthScheme {
    None,         // no credential header (an empty api_key behaves this way too)
    Bearer,       // Authorization: Bearer <api_key>
    CustomHeader, // <header_name>: <api_key>
};

NLOHMANN_JSON_SERIALIZE_ENUM(AuthScheme, {
    {AuthScheme::None, "none"},
    {AuthScheme::Bearer, "bearer"},
    {AuthScheme::CustomHeader, "custom_header"},
})

// Credentials for one provider endpoint. An empty api_key never produces a
// credential header regardless of scheme — local gateways need none.
struct EndpointAuth {
    AuthScheme scheme = AuthScheme::Bearer;
    std::string api_key;
    // Header name for AuthScheme::CustomHeader (ignored by the other
    // schemes); the default matches the Anthropic-style x-api-key.
    std::string header_name = "x-api-key";
    std::optional<nlohmann::json> extras;
};

inline void to_json(nlohmann::json& j, const EndpointAuth& a) {
    j = nlohmann::json{
        {"scheme", a.scheme},
        {"api_key", a.api_key},
        {"header_name", a.header_name},
    };
    if (a.extras) j["extras"] = *a.extras;
}

inline void from_json(const nlohmann::json& j, EndpointAuth& a) {
    if (auto it = j.find("scheme"); it != j.end()) it->get_to(a.scheme);
    if (auto it = j.find("api_key"); it != j.end()) it->get_to(a.api_key);
    if (auto it = j.find("header_name"); it != j.end())
        it->get_to(a.header_name);
    // JSON null reads as ABSENT: an engaged optional must never hold null,
    // or to_json's engagement gate would re-emit "extras": null and break
    // the never-null round-trip (the same guard detail::read_optional
    // applies in model_io.hpp).
    if (auto it = j.find("extras"); it != j.end() && !it->is_null())
        a.extras = *it;
    else a.extras.reset();
}

// ---- endpoint ------------------------------------------------------------

// One model provider: where to send, how to authenticate, and the transport
// headers to carry.
struct ModelEndpoint {
    // Provider base URL, parsed leniently by the consumer: scheme optional
    // (https assumed; http allowed for local backends), explicit port kept,
    // path prefix preserved and prepended to request_path, trailing slash
    // tolerated. The one strict requirement: it must resolve to a host.
    std::string base_url;
    // Request path appended to the base_url prefix. OpenAI-style default;
    // Anthropic-style backends use "/v1/messages".
    std::string request_path = "/chat/completions";
    EndpointAuth auth;
    std::string user_agent = "simplex-cpp";
    // User-supplied extra headers, applied after the standard ones so they
    // may override defaults (e.g. a provider version header).
    std::map<std::string, std::string> extra_headers;
    std::optional<nlohmann::json> extras;
};

inline void to_json(nlohmann::json& j, const ModelEndpoint& e) {
    j = nlohmann::json{
        {"base_url", e.base_url},
        {"request_path", e.request_path},
        {"auth", e.auth},
        {"user_agent", e.user_agent},
        {"extra_headers", e.extra_headers},
    };
    if (e.extras) j["extras"] = *e.extras;
}

inline void from_json(const nlohmann::json& j, ModelEndpoint& e) {
    if (auto it = j.find("base_url"); it != j.end()) it->get_to(e.base_url);
    if (auto it = j.find("request_path"); it != j.end())
        it->get_to(e.request_path);
    if (auto it = j.find("auth"); it != j.end()) it->get_to(e.auth);
    if (auto it = j.find("user_agent"); it != j.end())
        it->get_to(e.user_agent);
    if (auto it = j.find("extra_headers"); it != j.end())
        it->get_to(e.extra_headers);
    // JSON null reads as ABSENT — see EndpointAuth::from_json's extras note.
    if (auto it = j.find("extras"); it != j.end() && !it->is_null())
        e.extras = *it;
    else e.extras.reset();
}

} // namespace model_io
