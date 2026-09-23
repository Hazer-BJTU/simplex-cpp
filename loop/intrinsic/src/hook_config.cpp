#include "loop/intrinsic/hook_config.hpp"

#include "logging/logger.hpp"
#include "yamlconfig/yaml_json.hpp"

#include <cstdlib>
#include <format>
#include <system_error>
#include <utility>

#ifndef SIMPLEX_LOOP_HOOK_SOURCE_ROOT
#error "loop_intrinsic must define SIMPLEX_LOOP_HOOK_SOURCE_ROOT"
#endif

namespace loop::intrinsic {
namespace {

using nlohmann::json;

[[noreturn]] void fail(const std::filesystem::path& file,
                       std::string_view field,
                       std::string_view reason) {
    throw HookConfigError(std::format(
        "in {}: at /{}: {}", file.string(), field, reason));
}

std::string required_text(const json& document,
                          const std::filesystem::path& file,
                          std::string_view field) {
    const auto found = document.find(field);
    if (found == document.end()) {
        fail(file, field, "required field is missing");
    }
    if (!found->is_string()) {
        fail(file, field, "must be a string");
    }

    const std::string value = found->get<std::string>();
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        fail(file, field, "must not be empty");
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool portable_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (const unsigned char ch : name) {
        if ((ch >= 'a' && ch <= 'z')
            || (ch >= 'A' && ch <= 'Z')
            || (ch >= '0' && ch <= '9')
            || ch == '_' || ch == '-') {
            continue;
        }
        return false;
    }
    return true;
}

std::filesystem::path beside_executable(std::string_view name) {
    std::error_code failure;
    const std::filesystem::path executable =
        std::filesystem::read_symlink("/proc/self/exe", failure);
    if (failure || executable.empty()) {
        return {};
    }
    return executable.parent_path() / "schemas" / "loop" / name;
}

bool directory_exists(const std::filesystem::path& directory) {
    std::error_code failure;
    return !directory.empty()
        && std::filesystem::is_directory(directory, failure);
}

} // namespace

HookConfigError::HookConfigError(const std::string& message)
    : std::runtime_error(message) {
}

HookConfigError::~HookConfigError() = default;

HookConfig load_hook_config(const std::filesystem::path& file,
                            std::string_view expected_name) {
    if (!portable_name(expected_name)) {
        throw std::invalid_argument("expected hook name must be one portable path segment");
    }

    json document;
    try {
        document = yamlconfig::load_file(file);
    } catch (const yamlconfig::YamlConfigError& failure) {
        throw HookConfigError(failure.what());
    } catch (const std::exception& failure) {
        throw HookConfigError(std::format(
            "in {}: cannot read hook config ({})", file.string(), failure.what()));
    }

    if (!document.is_object()) {
        fail(file, "", "hook config must be a YAML mapping");
    }
    for (auto it = document.begin(); it != document.end(); ++it) {
        if (it.key() != "name" && it.key() != "description" && it.key() != "config") {
            fail(file, it.key(), "unknown field; expected name, description or config");
        }
    }

    HookConfig result;
    result.name = required_text(document, file, "name");
    if (!portable_name(result.name)) {
        fail(file, "name", "must use only ASCII letters, digits, '_' or '-'");
    }
    if (result.name != expected_name) {
        fail(file, "name", std::format(
            "expected {}, got {}", expected_name, result.name));
    }
    result.description = required_text(document, file, "description");

    const auto config = document.find("config");
    if (config == document.end()) {
        fail(file, "config", "required field is missing; use {} for no options");
    }
    if (!config->is_object()) {
        fail(file, "config", "must be a mapping");
    }
    result.config = *config;
    return result;
}

std::optional<HookConfig> try_load_hook_config(
    const std::filesystem::path& file,
    std::string_view expected_name) {
    try {
        return load_hook_config(file, expected_name);
    } catch (const std::exception& failure) {
        logging::Logger::error(std::format(
            "loop hook config rejected: {} — hook not registered", failure.what()));
        return std::nullopt;
    }
}

std::shared_ptr<LoopHookInterface> try_create_hook(
    const std::filesystem::path& file,
    std::string_view expected_name,
    const HookFactory& factory) {
    std::optional<HookConfig> config = try_load_hook_config(file, expected_name);
    if (!config) {
        return nullptr;
    }

    try {
        if (!factory) {
            throw std::invalid_argument("hook factory is empty");
        }
        std::shared_ptr<LoopHookInterface> hook = factory(std::move(*config));
        if (!hook || hook->name() != expected_name) {
            throw std::invalid_argument("hook factory returned a null or mismatched hook");
        }
        return hook;
    } catch (const std::exception& failure) {
        logging::Logger::error(std::format(
            "loop hook {} rejected: {} — hook not registered",
            expected_name, failure.what()));
        return nullptr;
    } catch (...) {
        logging::Logger::error(std::format(
            "loop hook {} rejected: unknown exception — hook not registered",
            expected_name));
        return nullptr;
    }
}

std::filesystem::path hook_config_file(std::string_view hook_name) {
    if (!portable_name(hook_name)) {
        throw std::invalid_argument("hook name must be one portable path segment");
    }

    if (const char* override_root = std::getenv("SIMPLEX_LOOP_HOOK_SCHEMA_DIR");
        override_root != nullptr && *override_root != '\0') {
        return std::filesystem::path(override_root) / hook_name / "config.yaml";
    }
    if (const std::filesystem::path installed = beside_executable(hook_name);
        directory_exists(installed)) {
        return installed / "config.yaml";
    }
    return std::filesystem::path(SIMPLEX_LOOP_HOOK_SOURCE_ROOT)
        / hook_name / "schemas" / "config.yaml";
}

IntrinsicLoopHook::IntrinsicLoopHook(HookConfig config)
    : config_(std::move(config)) {
    if (!portable_name(config_.name)) {
        throw std::invalid_argument("intrinsic hook config has an invalid name");
    }
    if (config_.description.empty() || !config_.config.is_object()) {
        throw std::invalid_argument("intrinsic hook config is incomplete");
    }
}

IntrinsicLoopHook::~IntrinsicLoopHook() = default;

std::string_view IntrinsicLoopHook::name() const noexcept {
    return config_.name;
}

const HookConfig& IntrinsicLoopHook::config() const noexcept {
    return config_;
}

} // namespace loop::intrinsic
