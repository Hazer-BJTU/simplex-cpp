#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "io/client.hpp"
#include "dataclass/prompt_template.hpp"
#include "load/archives.hpp"

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
    /** Parsed prompt for a new session, read from the installation directory;
     * restored snapshots retain their own prompt instead. */
    model_io::PromptTemplate system_prompt;
    /** Startup-loaded internal user instruction for a compact operation, read
     * from the installation directory and rendered to Markdown once. */
    std::string compact_prompt;
    /** Resolved archive directory; each compact operation adds a child. */
    std::filesystem::path memory;
    MemoryRetention memory_retention;
    /** Refreshed from startup configuration even for restored sessions. */
    RuntimeEnvironment environment;
    bool persistence = true;
    /** Direct session root; the worker never appends a session ID. */
    std::filesystem::path storage;
    /** Resolved snapshot directory below storage (persistence.state). */
    std::filesystem::path state_directory;
    bool restore = true;
    bool save_step = true;
    bool save_run = true;
    bool save_shutdown = true;
    bool readable = false;
};

/** Parse and validate startup fields before loading native code or opening IO.
 * Unknown keys remain tolerated. Only the selected provider's credentials are
 * expanded. Explicit paths resolve against the absolute configuration directory,
 * except persistence.state and persistence.memory, which are relative to the
 * direct persistence.directory root.
 *
 * worker.system_prompt_file and worker.compact_prompt_file are the mirror image
 * of that rule: they resolve against the installation directory — the running
 * executable's own directory — because a prompt is an asset of the deployed
 * worker rather than of the session that names it. They must be nonempty
 * relative paths without parent traversal; absolute paths are refused instead
 * of interpreted. Omitting either selects prompts/coding_agent.yaml or
 * prompts/operations/compact.yaml below the same root.
 *
 * Errors identify fields, never expanded credential values.
 */
Configuration parse_configuration(nlohmann::json document, std::filesystem::path directory);

/** The same parse with an explicit installation directory, for a host that
 * stages the worker's assets elsewhere and for tests that need a writable root.
 * The two-argument form and read_configuration use the running executable's
 * directory, which is the only root a deployed worker can be sure of.
 */
Configuration parse_configuration(nlohmann::json document, std::filesystem::path directory,
    std::filesystem::path installation);
Configuration read_configuration(const std::filesystem::path& file);

/**
 * Read and validate a standalone PromptTemplate YAML document synchronously.
 * Required sections are ordered immutable, growing, then volatile; names must
 * be unique and nonempty. The skill.* namespace, environment.runtime, signature.runtime, and memory.runtime are host-owned.
 * Unknown fields are tolerated, but malformed known fields are rejected. Empty
 * sections are permitted. Errors include the filename; no fallback is applied.
 */
model_io::PromptTemplate read_system_prompt(const std::filesystem::path& file);

/** Strict WebSocket URL parsing shared by startup and protocol tests. */
endpoint::ResolvedEndpoint websocket_endpoint(const std::string& url);
} // namespace load
