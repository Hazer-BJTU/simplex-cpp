#pragma once

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "dataclass/model_io.hpp"

namespace load {

/** JSON is a restorable snapshot; Readable is a lossy Markdown export. */
enum class StateFormat {
    Json,
    Readable,
};

/** Limits apply to each JSON preview, never to the conversation as a whole. */
struct ReadableOptions {
    /// Maximum UTF-8 bytes retained from each JSON string or object key.
    std::size_t max_json_string_bytes = 1024;
    /// Maximum entries shown in each JSON object or array.
    std::size_t max_json_items = 32;
    /// Maximum nested containers expanded from a JSON preview's root.
    std::size_t max_json_depth = 6;
    /// Maximum bytes of JSON preview, excluding fences and omission notices.
    std::size_t max_json_block_bytes = 8192;
};

/** File, serialization, or snapshot decoding failure, including the file path. */
class PersistenceError : public std::runtime_error {
public:
    explicit PersistenceError(const std::string& message, bool published = false);
    ~PersistenceError() override;

    /// True only when saving replaced the file but directory sync failed.
    /// The new snapshot is visible, but its crash durability is uncertain.
    /// Load failures and save failures before replacement return false.
    bool published() const noexcept { return published_; }

private:
    bool published_;
};

/**
 * Save a complete JSON snapshot or a human-readable Markdown view.
 *
 * @param file Explicit destination, resolved against the current directory if
 * relative. Parent directories are created when missing. No session filename
 * is inferred, and no startup YAML policy is interpreted by this function.
 * @param state Caller-owned state, read by const reference. The caller must
 * serialize this synchronous operation with state mutation. Session identity,
 * timestamps, progress, and tool results are preserved without modification.
 * @param format Json uses existing dataclass serialization without clipping.
 * Readable walks the state in chronological order and cannot be loaded back.
 * @param options Positive preview limits for Readable; depth must be at most
 * 64 and block size at least 64 bytes. Ignored for Json. Ordinary text remains
 * complete; JSON objects/arrays encoded in text receive the same preview limits.
 * Binary content is summarized instead of embedding base64.
 *
 * Publication delegates to fileio::atomic_write in utils/fileio. On the
 * supported POSIX runtime, writes use an exclusively created, mode-0600
 * temporary file in the destination directory, then fsync, close, and rename.
 * Failure before rename preserves any previous destination and removes the
 * temporary file. Replacement does not follow a destination symlink. The parent
 * directory is synced after rename; if that sync fails, the error explicitly
 * reports published() == true because replacement has already occurred. Newly
 * created ancestor directories are not individually synced. Concurrent writers require host
 * coordination; atomic replacement does not merge their states.
 *
 * JSON serialization builds one JSON representation, but never copies the
 * AgentInputState. Markdown streams sections and bounds each JSON preview.
 * The total Markdown output size is not bounded; ordinary text remains complete.
 * Neither format resumes a loop, invokes tools, or subscribes to events.
 */
void save_state(
    const std::filesystem::path& file,
    const model_io::AgentInputState& state,
    StateFormat format = StateFormat::Json,
    const ReadableOptions& options = {});

/**
 * Load a JSON snapshot into a newly constructed AgentInputState.
 *
 * The document must be one complete JSON object with meta and system_prompt
 * objects, and tools and turns arrays. Unknown fields and optional legacy
 * fields retain the dataclass decoder's compatibility rules. Malformed input,
 * a missing file, and Markdown exports throw PersistenceError; none are treated
 * as an empty session. Decoding never mutates a caller-owned state. The host
 * remains responsible for loop recovery validation before execution, including
 * blocked phases and pending tool results. This function performs no replay.
 */
[[nodiscard]] model_io::AgentInputState load_state(
    const std::filesystem::path& file);

} // namespace load
