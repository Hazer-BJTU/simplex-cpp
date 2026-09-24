#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>
#include "io/client.hpp"

namespace load {

/** Parsed startup settings. Runtime configuration, never a session snapshot. */
struct Configuration {
    nlohmann::json document;
    std::filesystem::path directory;
    std::string provider;
    nlohmann::json model;
    endpoint::ResolvedEndpoint client;
    io::ClientOptions queues;
    intercom::StableWebSocketOptions transport;
    std::optional<endpoint::ResolvedEndpoint> confirmation;
    std::chrono::milliseconds confirmation_timeout{120000};
    std::size_t event_capacity = 256;
    std::size_t max_exchanges = 12;
    std::string system_prompt = "You are a helpful assistant. Follow the available tool guidance.";
    bool persistence = true;
    std::filesystem::path storage;
    bool restore = true;
    bool save_step = true;
    bool save_run = true;
    bool save_shutdown = true;
    bool readable = false;
};

/** Parse and validate startup fields before loading native code or opening IO.
 * Unknown keys remain tolerated. Only the selected provider's credentials are
 * expanded. Explicit paths resolve against the absolute configuration directory.
 * Errors identify fields, never expanded credential values.
 */
Configuration parse_configuration(nlohmann::json document, std::filesystem::path directory);
Configuration read_configuration(const std::filesystem::path& file);

/** Strict WebSocket URL parsing shared by startup and protocol tests. */
endpoint::ResolvedEndpoint websocket_endpoint(const std::string& url);
} // namespace load
