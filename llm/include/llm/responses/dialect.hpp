#pragma once

#include <memory>

#include <nlohmann/json.hpp>

#include "dataclass/endpoint_config.hpp"

namespace llm::responses {

/**
 * Provider-specific spelling around the canonical Responses API.
 *
 * The common model owns conversation mapping, streaming and assembly. A
 * dialect supplies routing defaults and may rewrite the finished request body
 * or one decoded event. Rewrites are deliberately JSON-level: provider-only
 * types never cross the LLM plugin ABI.
 */
class ResponsesDialect {
public:
    virtual ~ResponsesDialect() = default;

    virtual model_io::ModelEndpoint default_endpoint() const {
        model_io::ModelEndpoint endpoint;
        endpoint.request_path = "/v1/responses";
        endpoint.auth.scheme = model_io::AuthScheme::Bearer;
        return endpoint;
    }

    virtual void transform_request(nlohmann::json&) const {}

    virtual nlohmann::json normalize_event(nlohmann::json event) const {
        return event;
    }
};

using ResponsesDialectPtr = std::shared_ptr<const ResponsesDialect>;

inline ResponsesDialectPtr default_dialect() {
    static const auto dialect = std::make_shared<const ResponsesDialect>();
    return dialect;
}

} // namespace llm::responses
