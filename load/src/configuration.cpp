#include "load/configuration.hpp"
#include "yamlconfig/yaml_json.hpp"
#include <cstdlib>
#include <charconv>
#include <limits>
#include <stdexcept>

namespace load {
namespace {
using Json = nlohmann::json;

const Json& object(const Json& parent, const char* key) {
    static const Json empty = Json::object();
    auto it = parent.find(key);
    if (it == parent.end()) return empty;
    if (!it->is_object()) throw std::invalid_argument(std::string(key) + " must be a mapping");
    return *it;
}

std::string text(const Json& parent, const char* key, std::string fallback = {}) {
    auto it = parent.find(key);
    if (it == parent.end()) return fallback;
    if (!it->is_string()) throw std::invalid_argument(std::string(key) + " must be a string");
    return it->get<std::string>();
}

std::size_t number(const Json& parent, const char* key, std::size_t fallback, bool zero = false) {
    auto it = parent.find(key);
    if (it == parent.end()) return fallback;
    if (!it->is_number_integer() || (!it->is_number_unsigned() && it->get<long long>() < 0))
        throw std::invalid_argument(std::string(key) + " must be a nonnegative integer");
    const auto value = it->get<std::uint64_t>();
    if ((!zero && value == 0) || value > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument(std::string(key) + " is out of range");
    return value;
}

bool flag(const Json& parent, const char* key, bool fallback) {
    auto it = parent.find(key);
    if (it == parent.end()) return fallback;
    if (!it->is_boolean()) throw std::invalid_argument(std::string(key) + " must be boolean");
    return it->get<bool>();
}

/** One pass only: substituted values are never parsed as expressions. */
std::string expand(const std::string& value) {
    std::string result;
    for (std::size_t i = 0; i < value.size();) {
        if (value.compare(i, 2, "$$") == 0) {
            result += '$';
            i += 2;
        } else if (value.compare(i, 2, "${") == 0) {
            auto end = value.find('}', i + 2);
            if (end == std::string::npos) throw std::invalid_argument("unclosed credential variable");
            auto name = value.substr(i + 2, end - i - 2);
            if (name.empty() || name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
                throw std::invalid_argument("invalid credential variable name");
            const char* environment = std::getenv(name.c_str());
            if (!environment || !*environment) throw std::invalid_argument("required credential variable is unset or empty");
            result += environment;
            i = end + 1;
        } else {
            result += value[i++];
        }
    }
    return result;
}
} // namespace

endpoint::ResolvedEndpoint websocket_endpoint(const std::string& url) {
    if (!(url.starts_with("ws://") || url.starts_with("wss://"))
        || url.find_first_of(" \t\r\n#") != std::string::npos)
        throw std::invalid_argument("endpoint requires a complete ws:// or wss:// URL");
    const auto begin = url.find("://") + 3;
    const auto end = url.find_first_of("/?", begin);
    const auto authority = url.substr(begin, end - begin);
    if (authority.empty() || authority.find('@') != std::string::npos)
        throw std::invalid_argument("endpoint requires a host without userinfo");
    model_io::ModelEndpoint config;
    config.base_url = url.substr(0, end);
    config.request_path = end == std::string::npos ? "/" : url.substr(end);
    if (config.request_path.starts_with("?")) config.request_path.insert(0, "/");
    auto resolved = endpoint::resolve_endpoint(config);
    if (resolved.host.empty()) throw std::invalid_argument("endpoint requires a host");
    unsigned int port = 0;
    const auto [position, error] = std::from_chars(
        resolved.port.data(), resolved.port.data() + resolved.port.size(), port);
    if (error != std::errc{} || position != resolved.port.data() + resolved.port.size()
        || port == 0 || port > 65535)
        throw std::invalid_argument("endpoint requires a numeric TCP port in 1..65535");
    return resolved;
}

Configuration parse_configuration(Json document, std::filesystem::path directory) {
    if (!document.is_object() || !directory.is_absolute())
        throw std::invalid_argument("configuration requires a mapping and absolute base directory");
    Configuration result;
    result.document = document;
    result.directory = directory;
    auto name = text(document, "driver_model");
    const auto& providers = object(document, "providers");
    if (name.empty() || !providers.contains(name) || !providers.at(name).is_object())
        throw std::invalid_argument("driver_model must name a providers mapping");
    const auto& selected = providers.at(name);
    result.provider = text(selected, "plugin", name);
    result.model = object(selected, "config");
    for (const char* reserved : {"model", "provider", "endpoint", "retry"}) {
        if (result.model.contains(reserved))
            throw std::invalid_argument("provider config contains a host-owned field");
    }
    const auto model = text(selected, "model");
    if (model.empty() || result.provider.empty()) throw std::invalid_argument("provider and model must be nonempty");
    result.model["model"] = model;
    auto endpoint = object(selected, "endpoint");
    const auto& auth = object(endpoint, "auth");
    auto scheme = text(auth, "scheme", "bearer");
    if (scheme != "none" && scheme != "bearer" && scheme != "custom_header")
        throw std::invalid_argument("invalid provider auth scheme");
    for (const char* field : {"base_url", "request_path", "user_agent"}) {
        if (endpoint.contains(field)) (void)text(endpoint, field);
    }
    (void)text(auth, "header_name");
    if (auth.contains("api_key")) endpoint["auth"]["api_key"] = expand(text(auth, "api_key"));
    const auto headers = object(endpoint, "extra_headers");
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        if (!it->is_string()) throw std::invalid_argument("extra_headers values must be strings");
        endpoint["extra_headers"][it.key()] = expand(it->get<std::string>());
    }
    result.model["endpoint"] = std::move(endpoint);
    const auto& retry = object(selected, "retry");
    const auto initial = number(retry, "initial_backoff_ms", 500);
    const auto maximum = number(retry, "max_backoff_ms", 120000);
    if (maximum < initial) throw std::invalid_argument("model backoff maximum is below initial");
    result.model["retry"] = {{"max_attempts", number(retry, "max_attempts", 3, true)},
        {"initial_backoff_ms", initial}, {"max_backoff_ms", maximum}};

    const auto& client = object(document, "client");
    result.client = websocket_endpoint(text(client, "endpoint"));
    result.queues.payload_capacity = number(client, "payload_capacity", 64);
    result.queues.signal_capacity = number(client, "signal_capacity", 64);
    const auto& transport = object(client, "transport");
    result.transport.write_capacity = number(transport, "write_capacity", 64);
    result.transport.initial_backoff = std::chrono::milliseconds(number(transport, "initial_backoff_ms", 250));
    result.transport.max_backoff = std::chrono::milliseconds(number(transport, "max_backoff_ms", 10000));
    result.transport.idle_timeout = std::chrono::seconds(number(transport, "idle_timeout_seconds", 0, true));
    if (result.transport.max_backoff < result.transport.initial_backoff)
        throw std::invalid_argument("client backoff maximum is below initial");
    const auto& security = object(document, "security");
    const auto& confirmation = object(security, "confirmation");
    if (security.contains("confirmation")) {
        result.confirmation = websocket_endpoint(text(confirmation, "endpoint"));
        result.confirmation_timeout = std::chrono::milliseconds(number(confirmation, "timeout_ms", 120000));
    }
    const auto& worker = object(document, "worker");
    result.event_capacity = number(worker, "event_capacity", 256);
    result.max_exchanges = number(worker, "max_exchanges", 12);
    result.system_prompt = text(worker, "system_prompt", result.system_prompt);
    const auto& storage = object(document, "persistence");
    result.persistence = flag(storage, "enabled", true);
    auto location = text(storage, "directory", "./data/sessions");
    if (location.empty()) throw std::invalid_argument("persistence directory must be nonempty");
    result.storage = (directory / location).lexically_normal();
    if (text(storage, "format", "json") != "json") throw std::invalid_argument("persistence format must be json");
    auto restore = text(storage, "restore", "if_present");
    if (restore != "if_present" && restore != "never") throw std::invalid_argument("invalid restore policy");
    result.restore = restore == "if_present";
    const auto& save = object(storage, "save");
    result.save_step = flag(save, "on_step_finished", true);
    result.save_run = flag(save, "on_run_finished", true);
    result.save_shutdown = flag(save, "on_shutdown", true);
    result.readable = flag(storage, "readable", false);
    return result;
}

Configuration read_configuration(const std::filesystem::path& file) {
    return parse_configuration(yamlconfig::load_file(file), std::filesystem::absolute(file).parent_path());
}
} // namespace load
