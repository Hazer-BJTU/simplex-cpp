#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "load/retention.hpp"
#include "persistence/store.hpp"

/**
 * On-disk layout of the JSON backend's compact archives.
 *
 * One attempt is a directory below the configured archive root:
 *
 *     <root>/<archive-id>/state.json     complete original AgentInputState
 *     <root>/<archive-id>/manifest.json  attempt identity, written last
 *
 * The manifest is the publication marker. The payload is written first through
 * the JSON codec's atomic replacement, and the manifest is then published
 * atomically; recognition requires both files, the recorded payload size and a
 * matching store identity. A payload left behind by a crash or a failed manifest
 * write is therefore incomplete evidence, never a retention candidate and never
 * a restorable archive.
 *
 * A directory that an older worker filled with a single `state.md` is reported
 * as `legacy_readable`: write-only Markdown that cannot be decoded back into
 * state. Those artifacts are never retention candidates and are never deleted
 * automatically, because no worker can reconstruct or replace their content.
 */
namespace persistence::json_archive {

/** Complete JSON state of one archived attempt; the restorable payload. */
inline constexpr const char* state_file = "state.json";
/** Publication marker, written after the payload. */
inline constexpr const char* manifest_file = "manifest.json";
/** Markdown payload written by workers before #187 PR-B. */
inline constexpr const char* legacy_file = "state.md";
/** Manifest format label; independent of the state schema version. */
inline constexpr const char* manifest_format = "simplex-compact-archive";
/** Manifest layout version understood by this worker. */
inline constexpr std::uint64_t manifest_version = 1;

/** True for the ordinal/timestamp/UUID directory name of one archive. */
bool is_archive_name(std::string_view name);

/**
 * Plan a never-reused archive directory below `root` without creating it.
 *
 * The ordinal derives from existing entries so restarts and wall-clock changes
 * cannot reorder archives. A reserved attempt keeps its ordinal even when its
 * publication fails, and the session's ownership lock serializes cooperating
 * worker processes. A run identity that would not form a recognized archive name
 * is a caller error rather than an unusable directory.
 */
std::filesystem::path plan_directory(
    const std::filesystem::path& root, const std::string& run_id);

/**
 * Publish the complete archive into an already created attempt directory.
 *
 * Writes `state.json` first and the manifest last, so a partially published
 * attempt is recognizable as incomplete from its contents alone. Codec,
 * filesystem and publication failures propagate unchanged: the caller maps them
 * to the store outcome that tells whether a payload may already be visible.
 */
void publish(
    const std::filesystem::path& directory, const std::string& session_id,
    const model_io::AgentInputState& original);

/** Classification of one existing archive directory. */
struct Recognized {
    ArchiveKind kind = ArchiveKind::LegacyReadable;
    std::filesystem::path payload;
};

/**
 * Classify one directory as a complete JSON archive or as legacy Markdown.
 *
 * Returns nothing for another name, a symlink, extra or missing files, an
 * unreadable manifest, a foreign session identity or a size mismatch. Callers
 * treat that as "not this store's evidence" and leave the directory alone.
 */
std::optional<Recognized> recognize(
    const std::filesystem::path& directory, const std::string& session_id);

/**
 * Map a path recorded in retained state to the archive directory that owns it.
 *
 * Retained state records the payload file of an earlier attempt, or the whole
 * archive directory. Both forms resolve to a recognized archive; a nested,
 * foreign or unrecognized path resolves to nothing so that no unrelated file is
 * ever protected or deleted on its behalf.
 */
std::optional<ArchiveRef> reference_at(
    const std::filesystem::path& root, const std::filesystem::path& path,
    const std::string& session_id);

/**
 * Apply the retention target to complete JSON archives below `root`.
 *
 * Policy is unchanged from the pre-#187 worker: the current attempt and the
 * archives named by retained state are always protected, other recognized
 * archives are kept newest first, and nothing else is touched. Legacy Markdown,
 * unknown files, symlinks and incomplete archives are not candidates at all.
 * Filesystem errors propagate after any completed deletions.
 */
CleanupResult prune(
    const std::filesystem::path& root, const std::string& session_id,
    const ArchiveRef& current, const std::vector<ArchiveRef>& referenced,
    const load::MemoryRetention& policy);

} // namespace persistence::json_archive
