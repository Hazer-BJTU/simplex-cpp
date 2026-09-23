// A real plugin whose prepare/execute contract depends on ownership identity.
#include "tools/extensions/plugin.hpp"
#include "extensions/plugin_magic.hpp"

#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/dll/alias.hpp>

#include <fstream>

namespace ownership_fixture {

class ProbeTool final : public tools::ToolInterface {
public:
    const model_io::Invocable& get_details() const noexcept override {
        return details_;
    }

private:
    model_io::Invocable details_{
        "ownership_probe", "Test plugin ownership", nlohmann::json::object(), {}, {}};
};

class ProbeSet final : public tools::ToolSet {
public:
    explicit ProbeSet(const tools::extensions::ToolSetConfig& config)
        : destruction_marker_(config.config.at("destruction_marker").get<std::string>()),
          storage_(std::make_shared<Storage>()) {}

    ~ProbeSet() override {
        // The host checks this marker to detect cycles caused by retained handles.
        std::ofstream marker(destruction_marker_);
        marker << "destroyed";
    }

    std::string_view name() const noexcept override {
        return "ownership_probe";
    }

    std::vector<model_io::Invocable> get_tools() const override {
        return {storage_->tool.get_details()};
    }

    ToolHandle dispatch(const model_io::InvokeQuery& query) const override {
        if (query.name != "ownership_probe") {
            return nullptr;
        }
        // The tool is a subobject: both its aliasing pointer and the storage's
        // control block must survive the trip through the host wrapper.
        return ToolHandle(storage_, &storage_->tool);
    }

    ToolHandle prepare(model_io::InvokeQuery& query) override {
        auto tool = dispatch(query);
        query.type = model_io::InvokeType::ReadOnly;
        query.security = model_io::InvokeSecurity::Trusted;
        prepared_ = tool;
        return tool;
    }

    boost::asio::awaitable<model_io::InvokeReturn> execute(
        ToolHandle tool, model_io::InvokeQuery query) override {
        const auto matches = [&] {
            return !prepared_.owner_before(tool) && !tool.owner_before(prepared_)
                && tool.get() == &storage_->tool;
        };
        const bool matched_before = matches();
        {
            std::ofstream started(destruction_marker_.string() + ".started");
            started << "executing";
        }
        co_await boost::asio::post(boost::asio::use_awaitable);
        const bool matched_after = matches();
        // This is legal plugin state. Retaining a host carrier here would form
        // a set -> carrier -> set cycle and prevent the destructor marker.
        retained_ = tool;
        co_return model_io::InvokeReturn{
            std::move(query),
            {model_io::ContentType::Text,
             matched_before && matched_after ? "ownership preserved" : "ownership changed",
             {}},
            {}};
    }

private:
    struct Storage {
        int prefix = 0;
        ProbeTool tool;
    };

    std::filesystem::path destruction_marker_;
    std::shared_ptr<Storage> storage_;
    std::weak_ptr<tools::ToolInterface> prepared_;
    ToolHandle retained_;
};

class Context final : public tools::extensions::ToolSetExtensionContext {
public:
    std::uint32_t abi_version() const noexcept override {
        return tools::extensions::kAbiVersion;
    }
    std::string_view name() const noexcept override {
        return "ownership_probe";
    }
};

std::unique_ptr<extension::ExtensionContext> context() {
    return std::make_unique<Context>();
}

std::unique_ptr<tools::ToolSet> create(const tools::extensions::ToolSetConfig& config) {
    return std::make_unique<ProbeSet>(config);
}

} // namespace ownership_fixture

BOOST_DLL_ALIAS(ownership_fixture::context, create_toolset_plugin)
BOOST_DLL_ALIAS(ownership_fixture::create, create_toolset)
SIMPLEX_EXPORT_PLUGIN_MAGIC;
