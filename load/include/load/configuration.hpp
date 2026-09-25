#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "io/client.hpp"
#include "dataclass/prompt_template.hpp"

namespace load {

/** Configured prompt hints only: no chdir, sandbox, or capability detection. */
struct RuntimeEnvironment {
    /** Empty means omitted; parsed relative paths use the config directory. */
    std::filesystem::path workspace;
    std::string platform;
    std::vector<std::string> software;
};

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
    std::size_t event_capacity = 1024;
    std::size_t max_exchanges = 512;
    /** Parsed prompt for a new session; restored snapshots retain their prompt. */
    model_io::PromptTemplate system_prompt;
    /** Refreshed from startup configuration even for restored sessions. */
    RuntimeEnvironment environment;
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

/**
 * Read and validate a standalone PromptTemplate YAML document synchronously.
 * Required sections are ordered immutable, growing, then volatile; names must
 * be unique and nonempty. The skill.* namespace, environment.runtime, and signature.runtime are host-owned.
 * Unknown fields are tolerated, but malformed known fields are rejected. Empty
 * sections are permitted. Errors include the filename; no fallback is applied.
 */
model_io::PromptTemplate read_system_prompt(const std::filesystem::path& file);

/** Strict WebSocket URL parsing shared by startup and protocol tests. */
endpoint::ResolvedEndpoint websocket_endpoint(const std::string& url);
} // namespace load
