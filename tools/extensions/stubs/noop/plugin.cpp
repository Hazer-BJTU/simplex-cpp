#include "tools/extensions/plugin.hpp"
#include "extensions/plugin_magic.hpp"

#include <boost/dll/alias.hpp>

#include <stdexcept>
#include <utility>

namespace tools::extensions::noop {

class NoopTool final : public tools::ToolInterface {
public:
    explicit NoopTool(const tools::extensions::ToolSetConfig& config)
        : details_(tools::extensions::load_tool(config, "noop_probe")) {}

    const model_io::Invocable& get_details() const noexcept override {
        return details_;
    }
    void write_attributes(model_io::InvokeQuery& query) const override {
        query.type = model_io::InvokeType::ReadOnly;
        query.security = model_io::InvokeSecurity::Trusted;
    }
    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery&) override {
        co_return model_io::Content{model_io::ContentType::Text, "noop", {}};
    }
private:
    model_io::Invocable details_;
};

class NoopToolSet final : public tools::ToolSet {
public:
    explicit NoopToolSet(const tools::extensions::ToolSetConfig& config)
        : tool_(std::make_shared<NoopTool>(config)),
          skill_(tools::extensions::load_skill(config)) {}
    std::string_view name() const noexcept override {
        return "noop_toolset";
    }
    std::vector<model_io::Invocable> get_tools() const override {
        return {tool_->get_details()};
    }
    ToolHandle dispatch(const model_io::InvokeQuery& query) const override {
        return query.name == tool_->get_details().name ? tool_ : nullptr;
    }
    std::optional<tools::ToolSetSkill> skill() const override {
        return skill_;
    }
private:
    ToolHandle tool_;
    std::optional<tools::ToolSetSkill> skill_;
};

class NoopContext final : public tools::extensions::ToolSetExtensionContext {
public:
    std::uint32_t abi_version() const noexcept override {
        return tools::extensions::kAbiVersion;
    }
    std::string_view name() const noexcept override {
        return "noop_toolset";
    }
};

std::unique_ptr<extension::ExtensionContext> create_toolset_plugin() {
    return std::make_unique<NoopContext>();
}

std::unique_ptr<tools::ToolSet> create_toolset(
    const tools::extensions::ToolSetConfig& config) {
    if (config.name != "noop_toolset" || !config.config.empty()) {
        throw std::invalid_argument("noop_toolset accepts no options");
    }
    return std::make_unique<NoopToolSet>(config);
}

} // namespace tools::extensions::noop

BOOST_DLL_ALIAS(tools::extensions::noop::create_toolset_plugin, create_toolset_plugin)
BOOST_DLL_ALIAS(tools::extensions::noop::create_toolset, create_toolset)
SIMPLEX_EXPORT_PLUGIN_MAGIC;
