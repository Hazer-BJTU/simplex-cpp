#include "llm/custom_chat/model.hpp"
#include "extensions/plugin_magic.hpp"
#include <boost/dll/alias.hpp>

namespace llm::custom_chat {
class Plugin final : public LLMModelExtensionContext {
public:
    std::uint32_t abi_version() const noexcept override { return LLM_PLUGIN_ABI_VERSION; }
    std::string_view name() const noexcept override { return "chat-completions"; }
};
std::unique_ptr<extension::ExtensionContext> create_llm_plugin() {
    return std::make_unique<Plugin>();
}
std::unique_ptr<LLMModel> create_llm_model(
    boost::asio::any_io_executor executor, const nlohmann::json& config) {
    return std::make_unique<CustomChatCompletionsModel>(std::move(executor), config);
}
} // namespace llm::custom_chat
BOOST_DLL_ALIAS(llm::custom_chat::create_llm_plugin, create_llm_plugin)
BOOST_DLL_ALIAS(llm::custom_chat::create_llm_model, create_llm_model)
SIMPLEX_EXPORT_PLUGIN_MAGIC;
