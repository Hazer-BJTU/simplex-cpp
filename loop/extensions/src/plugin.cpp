#include "loop/extensions/plugin.hpp"

#include "logging/logger.hpp"

#include <boost/dll/runtime_symbol_info.hpp>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace loop::extensions {
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

using Factory = std::unique_ptr<LoopHookInterface>(const intrinsic::HookConfig&);

} // namespace

LoopHookExtensionContext::~LoopHookExtensionContext() = default;

std::filesystem::path config_file(std::string_view name) {
    if (!portable(name)) {
        throw std::invalid_argument("invalid loop plugin name");
    }
    if (const char* root = std::getenv("SIMPLEX_LOOP_EXTENSION_SCHEMA_DIR");
        root && *root) {
        return std::filesystem::path(root) / name / "config.yaml";
    }
    return executable_directory() / "schemas" / "loop" / "extensions" / name / "config.yaml";
}

std::size_t LoopHookExtensionLoader::load(const std::filesystem::path& directory) {
    std::error_code error;
    const auto status = std::filesystem::status(directory, error);
    // Missing paths are an optional-plugin case, even when status() reports
    // ENOENT. Other failures (including symlink loops) must remain visible.
    if (status.type() == std::filesystem::file_type::not_found) {
        return 0;
    }
    if (error) {
        throw std::filesystem::filesystem_error(
            "failed to inspect extension directory", directory, error);
    }
    if (!std::filesystem::is_directory(status)) {
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
            auto context = std::dynamic_pointer_cast<LoopHookExtensionContext>(base);
            if (!context || !portable(context->name())) {
                throw std::runtime_error("invalid loop hook context or name");
            }
            if (std::any_of(contexts_.begin(), contexts_.end(), [&](const auto& old) {
                    return old->name() == context->name(); })) {
                throw std::runtime_error("duplicate loop hook plugin name");
            }
            (void)extension::detail::resolve_factory_alias<Factory>(
                context->get_library_ref(), kHookFactory);
            contexts_.push_back(std::move(context));
            ++added;
        } catch (const std::exception& failure) {
            logging::Logger::warning("loop plugin rejected: {}", failure.what());
        }
    }
    return added;
}

std::size_t LoopHookExtensionLoader::load_default() {
    return load(executable_directory() / "plugins" / "loop");
}

std::shared_ptr<LoopHookInterface> LoopHookExtensionLoader::create(
    std::string_view name,
    const std::filesystem::path& configuration) const {
    for (const auto& context : contexts_) {
        if (context->name() != name) {
            continue;
        }
        try {
            auto config = load_config(
                configuration.empty() ? config_file(name) : configuration, name);
            auto factory = extension::detail::resolve_factory_alias<Factory>(
                context->get_library_ref(), kHookFactory);
            auto object = factory(config);
            if (!object || object->name() != name) {
                throw std::runtime_error("hook factory returned null or wrong name");
            }
            auto library = context->get_library_ref();
            return std::shared_ptr<LoopHookInterface>(object.release(),
                [library](LoopHookInterface* p) { delete p; });
        } catch (const std::exception& failure) {
            logging::Logger::warning("loop plugin {} cannot be created: {}", name, failure.what());
            return nullptr;
        } catch (...) {
            logging::Logger::warning("loop plugin {} threw a non-standard exception", name);
            return nullptr;
        }
    }
    return nullptr;
}

} // namespace loop::extensions
