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

/** Selected provider factory and its validated, credential-expanded settings. */
struct ModelConfiguration {
    std::string provider;
    nlohmann::json model;
};

/** Parsed startup settings. Runtime configuration, never a session snapshot. */
struct Configuration {
    nlohmann::json document;
    std::filesystem::path directory;
    /** Driver factory and its validated, credential-expanded configuration. */
    std::string provider;
    nlohmann::json model;
    /** Optional independent model for the intrinsic modality_assist toolset.
     * Absence means no instance is constructed; never persisted in session state. */
    std::optional<ModelConfiguration> modality_assist_model;
    endpoint::ResolvedEndpoint client;
    io::ClientOptions queues;
    intercom::StableWebSocketOptions transport;
    std::optional<endpoint::ResolvedEndpoint> confirmation;
    std::chrono::milliseconds confirmation_timeout{120000};
    /** Optional base URL for one-shot remote tool requests, including the plan tool. */
    std::optional<endpoint::ResolvedEndpoint> hub_remote_call;
    /** Total request deadline for remote tool clients; retries are not implicit. */
    std::chrono::milliseconds hub_remote_call_timeout{120000};
    std::size_t event_capacity = 1024;
    std::size_t max_exchanges = 512;
    /** Zero disables automatic compaction; otherwise also converts exchange limits. */
    std::uint64_t auto_compact_threshold = 0;
    /** Finite attempt budget per admitted request, independent of archive retention. */
    std::size_t max_auto_compactions = 5;
    /** Dedicated startup-loaded handoff and private continuation instructions. */
    std::string auto_compact_prompt;
    std::string auto_compact_continue_prompt;
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
 * Unknown keys remain tolerated. Only providers selected by driver_model and
 * the optional modality_assist_model have their credentials
 * expanded. Explicit paths resolve against the absolute configuration directory,
 * except persistence.state and persistence.memory, which are relative to the
 * direct persistence.directory root.
 *
 * worker.system_prompt_file and worker.compact_prompt_file are the mirror image
 * of that rule: they resolve against the installation directory — the running
 * executable's own directory — because a prompt is an asset of the deployed
 * worker rather than of the session that names it. They must be nonempty
 * relative paths; rooted forms (a leading `/` or `\`, or a drive letter, which
 * a POSIX build and a Windows build would otherwise read differently) and `..`
 * components are refused instead of interpreted. Omitting either selects
 * prompts/coding_agent.yaml or prompts/operations/compact.yaml below the same
 * root.
 *
 * That containment is LEXICAL, not a filesystem sandbox: symlinks are not
 * resolved here, so a link below the installation directory still points
 * wherever it points. Resolution stays lexical on purpose — staging prompts
 * through links is a deployment choice, and canonicalising would make the rule
 * depend on the filesystem's state at read time.
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
