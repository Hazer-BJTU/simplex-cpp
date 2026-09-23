#include "tools/extensions/plugin.hpp"

#include "logging/logger.hpp"
#include "yamlconfig/yaml_json.hpp"

#include <boost/dll/runtime_symbol_info.hpp>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace tools::extensions {
namespace {

bool portable(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (unsigned char ch : name) {
        if (!(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z')
            && !(ch >= '0' && ch <= '9') && ch != '_' && ch != '-') {
            return false;
        }
    }
    return true;
}

std::filesystem::path executable_directory() {
    boost::system::error_code error;
    const auto executable = boost::dll::program_location(error);
    if (error) {
        throw boost::system::system_error(error);
    }
    return std::filesystem::path(executable.string()).parent_path();
}

using Factory = std::unique_ptr<ToolSet>(const ToolSetConfig&);

/** Keep both the product and each independently retained tool in its DSO. */
class LoadedToolSet final : public ToolSet {
public:
    explicit LoadedToolSet(std::shared_ptr<ToolSet> inner) : inner_(std::move(inner)) {}
    std::string_view name() const noexcept override {
        return inner_->name();
    }
    std::vector<model_io::Invocable> get_tools() const override {
        return inner_->get_tools();
    }
    std::optional<ToolSetSkill> skill() const override {
        return inner_->skill();
    }
    ToolHandle dispatch(const model_io::InvokeQuery& query) const override {
        return pin(inner_->dispatch(query));
    }
    ToolHandle prepare(model_io::InvokeQuery& query) override {
        return pin(inner_->prepare(query));
    }
    boost::asio::awaitable<model_io::InvokeReturn> execute(
        ToolHandle tool, model_io::InvokeQuery query) override {
        auto inner = inner_;
        co_return co_await inner->execute(std::move(tool), std::move(query));
    }
private:
    ToolHandle pin(ToolHandle tool) const {
        if (!tool) {
            return nullptr;
        }
        // The tool may escape prepare()/dispatch() and outlive the registry.
        // Capture its set so neither implementation nor its DSO disappears.
        ToolInterface* raw = tool.get();
        return ToolHandle(raw,
            [tool = std::move(tool), owner = inner_](ToolInterface*) mutable {
                // Destroy plugin-owned tools before releasing their owner and DSO.
                tool.reset();
                owner.reset();
            });
    }
    std::shared_ptr<ToolSet> inner_;
};

} // namespace

ToolSetExtensionContext::~ToolSetExtensionContext() = default;

std::filesystem::path schema_directory(std::string_view name) {
    if (!portable(name)) {
        throw std::invalid_argument("invalid tool plugin name");
    }
    if (const char* root = std::getenv("SIMPLEX_TOOL_EXTENSION_SCHEMA_DIR");
        root && *root) {
        return std::filesystem::path(root) / name;
    }
    return executable_directory() / "schemas" / "tools" / "extensions" / name;
}

ToolSetConfig load_config(const std::filesystem::path& directory,
                          std::string_view expected_name) {
    if (!portable(expected_name)) {
        throw std::invalid_argument("invalid tool plugin name");
    }
    const auto file = directory / "config.yaml";
    const auto document = yamlconfig::load_file(file);
    if (!document.is_object() || document.size() != 3
        || !document.contains("name") || !document.contains("description")
        || !document.contains("config") || !document["name"].is_string()
        || !document["description"].is_string() || !document["config"].is_object()) {
        throw std::runtime_error("invalid tool plugin config: " + file.string());
    }
    for (auto it = document.begin(); it != document.end(); ++it) {
        if (it.key() != "name" && it.key() != "description" && it.key() != "config") {
            throw std::runtime_error("unknown tool plugin config field: " + it.key());
        }
    }
    const std::string name = document["name"].get<std::string>();
    const std::string description = document["description"].get<std::string>();
    if (name != expected_name || description.find_first_not_of(" \t\r\n") == std::string::npos) {
        throw std::runtime_error("tool plugin config identity mismatch or empty description: " + file.string());
    }
    return {name, description, document["config"], directory};
}

model_io::Invocable load_tool(const ToolSetConfig& config, std::string_view tool_name) {
    if (!portable(tool_name)) {
        throw std::invalid_argument("invalid tool name");
    }
    const auto declaration = intrinsic::load_tool_declaration(
        config.schema_directory / (std::string(tool_name) + ".yaml"));
    if (declaration.name != tool_name) {
        throw std::runtime_error("tool declaration name does not match its filename");
    }
    return {declaration.name, declaration.description, declaration.argument_schema, {}, {}};
}

std::optional<ToolSetSkill> load_skill(const ToolSetConfig& config) {
    const auto file = config.schema_directory / "skill.yaml";
    if (!std::filesystem::exists(file)) {
        return std::nullopt;
    }
    return intrinsic::load_skill_declaration(file);
}

std::size_t ToolSetExtensionLoader::load(const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        return 0;
    }
    std::size_t added = 0;
    for (auto& base : extension::load_and_verify_directory(
             directory, extension::is_likely_dynamic_library,
             extension::same_tag_always{std::string(kContextFactory)})) {
        try {
            if (base->abi_version() != kAbiVersion) {
                throw std::runtime_error("ABI version mismatch");
            }
            auto context = std::dynamic_pointer_cast<ToolSetExtensionContext>(base);
            if (!context || !portable(context->name())) {
                throw std::runtime_error("invalid toolset context or name");
            }
            if (std::any_of(contexts_.begin(), contexts_.end(), [&](const auto& old) {
                    return old->name() == context->name(); })) {
                throw std::runtime_error("duplicate toolset plugin name");
            }
            (void)extension::detail::resolve_factory_alias<Factory>(
                context->get_library_ref(), kToolSetFactory);
            contexts_.push_back(std::move(context));
            ++added;
        } catch (const std::exception& failure) {
            logging::Logger::warning("tool plugin rejected: {}", failure.what());
        }
    }
    return added;
}

std::size_t ToolSetExtensionLoader::load_default() {
    return load(executable_directory() / "plugins" / "tools");
}

std::shared_ptr<ToolSet> ToolSetExtensionLoader::create(
    std::string_view name,
    const std::filesystem::path& configuration) const {
    for (const auto& context : contexts_) {
        if (context->name() != name) {
            continue;
        }
        try {
            auto config = load_config(
                configuration.empty() ? schema_directory(name) : configuration, name);
            auto factory = extension::detail::resolve_factory_alias<Factory>(
                context->get_library_ref(), kToolSetFactory);
            auto object = factory(config);
            if (!object || object->name() != name) {
                throw std::runtime_error("toolset factory returned null or wrong name");
            }
            auto library = context->get_library_ref();
            std::shared_ptr<ToolSet> owned(object.release(), [library](ToolSet* p) { delete p; });
            return std::make_shared<LoadedToolSet>(std::move(owned));
        } catch (const std::exception& failure) {
            logging::Logger::warning("tool plugin {} cannot be created: {}", name, failure.what());
            return nullptr;
        } catch (...) {
            logging::Logger::warning("tools plugin {} threw a non-standard exception", name);
            return nullptr;
        }
    }
    return nullptr;
}

} // namespace tools::extensions
