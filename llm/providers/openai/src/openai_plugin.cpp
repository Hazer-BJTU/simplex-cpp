// openai_plugin.cpp — the first concrete provider plugin for the llm stack.
//
// Thin by design: everything protocol-level lives in the shared llm_responses
// adapter (interpreter / stream handler / reader), so a provider plugin only
// supplies its dialect — endpoint defaults (base URL, Bearer auth, user
// agent) around the canonical /v1/responses shape — and exports the two
// standard aliases the loader resolves (see llm/models.hpp). Rewrites of the
// request body or individual events are not needed against the canonical
// OpenAI wire format; providers that deviate override the same two hooks.

#include "llm/models.hpp"
#include "llm/responses/model.hpp"

#include <memory>
#include <string_view>
#include <utility>

#include <boost/dll/alias.hpp>

namespace llm::openai {

namespace {

class OpenAIResponsesDialect final : public responses::ResponsesDialect {
public:
    model_io::ModelEndpoint default_endpoint() const override {
        model_io::ModelEndpoint endpoint;
        endpoint.base_url = "https://api.openai.com";
        endpoint.request_path = "/v1/responses";
        endpoint.auth.scheme = model_io::AuthScheme::Bearer;
        endpoint.user_agent = "simplex-cpp/openai-responses";
        return endpoint;
    }
};

responses::ResponsesDialectPtr openai_dialect() {
    static const auto dialect =
        std::make_shared<const OpenAIResponsesDialect>();
    return dialect;
}

class OpenAIResponsesModel final : public responses::ResponsesModel {
public:
    OpenAIResponsesModel(boost::asio::any_io_executor executor,
                         nlohmann::json config)
        : ResponsesModel(std::move(executor), std::move(config),
                         openai_dialect()) {}
};

class OpenAIPlugin final : public LLMModelExtensionContext {
public:
    std::uint32_t abi_version() const noexcept override {
        return LLM_PLUGIN_ABI_VERSION;
    }

    std::string_view name() const noexcept override { return "openai"; }
};

} // namespace

std::unique_ptr<extension::ExtensionContext> create_llm_plugin() {
    return std::make_unique<OpenAIPlugin>();
}

std::unique_ptr<LLMModel> create_llm_model(
    boost::asio::any_io_executor executor, const nlohmann::json& config) {
    return std::make_unique<OpenAIResponsesModel>(std::move(executor), config);
}

} // namespace llm::openai

BOOST_DLL_ALIAS(llm::openai::create_llm_plugin, create_llm_plugin)
BOOST_DLL_ALIAS(llm::openai::create_llm_model, create_llm_model)
