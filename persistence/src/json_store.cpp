#include "persistence/store.hpp"

#include "load/persistence.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace persistence {
namespace {

namespace fs = std::filesystem;

/**
 * Translate backend failures into the store contract.
 *
 * Codec and filesystem failures become StoreError so that callers can rely on
 * status() for recovery, including the published-but-unsynced case that the
 * codec reports through PersistenceError::published(). Programming errors such
 * as an invalid argument are deliberately not converted: they are caller bugs,
 * not storage outcomes, and must not be reported as a failed write.
 */
template<typename Function>
auto map_failures(Function&& function) -> decltype(function()) {
    try {
        return function();
    } catch (const StoreError&) {
        throw;
    } catch (const load::PersistenceError& error) {
        throw StoreError(error.what(), Backend::Json,
            error.published() ? CommitStatus::Unknown : CommitStatus::NotCommitted);
    } catch (const fs::filesystem_error& error) {
        throw StoreError(error.what(), Backend::Json, CommitStatus::NotCommitted);
    }
}

/**
 * Plan a never-reused archive directory without creating it.
 *
 * The ordinal derives from existing entries so restarts and wall-clock changes
 * cannot reorder archives. A reserved attempt keeps its ordinal even when its
 * publication fails, and the session's ownership lock serializes cooperating
 * worker processes.
 */
fs::path plan_archive_directory(const fs::path& root, const std::string& run) {
    std::uint64_t latest = 0;
    if (fs::exists(root)) {
        for (const auto& entry : fs::directory_iterator(root)) {
            const auto name = entry.path().filename().string();
            if (name.size() < 21 || name[20] != '-') continue;
            std::uint64_t ordinal = 0;
            const auto parsed = std::from_chars(name.data(), name.data() + 20, ordinal);
            if (parsed.ec == std::errc{} && parsed.ptr == name.data() + 20) {
                latest = std::max(latest, ordinal);
            }
        }
    }
    if (latest == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("compact archive sequence exhausted");
    }
    auto ordinal = std::to_string(latest + 1);
    ordinal.insert(0, 20 - ordinal.size(), '0');
    auto time = utc_timestamp();
    std::erase(time, ':');
    return root / (ordinal + "-" + time + "-" + run);
}

/**
 * Atomic JSON snapshots with Markdown compact evidence.
 *
 * The adapter keeps the historical worker layout so that #187 PR-A changes no
 * observable behavior: saves publish `state/state.json` through
 * load::save_state, and compact attempts archive a readable `state.md` before the
 * summary exchange. Archiving the complete original state, and retiring the
 * Markdown export, belong to PR-B; this file is where that format change lands.
 */
class JsonStore final : public StateStore {
public:
    JsonStore(std::string session, JsonStoreOptions options)
        : session_(std::move(session)),
          state_directory_(std::move(options.state_directory)),
          archive_root_(std::move(options.archive_root)) {}

    Backend backend() const noexcept override { return Backend::Json; }

    const std::string& session_id() const noexcept override { return session_; }

    std::optional<StoredState> load_current() override {
        return map_failures([this]() -> std::optional<StoredState> {
            const auto file = snapshot();
            if (!fs::exists(file)) return std::nullopt;
            StoredState stored;
            stored.state = load::load_state(file);
            stored.backend = Backend::Json;
            return stored;
        });
    }

    CommitReceipt checkpoint(
        const model_io::AgentInputState& state, Boundary boundary,
        std::optional<Revision> expected_revision) override {
        // JSON keeps no per-boundary record; an invalid label is still a caller
        // error worth rejecting before any write.
        (void)boundary_name(boundary);
        write_snapshot(state, expected_revision);
        return CommitReceipt{Backend::Json, CommitStatus::Committed, {}, std::nullopt};
    }

    ArchiveRef reserve_archive(const CompactAttempt& attempt) override {
        return map_failures([&] {
            const auto directory = plan_archive_directory(archive_root(), attempt.run_id);
            return ArchiveRef{session_, directory.filename().string(), Backend::Json,
                ArchiveKind::LegacyReadable, archive_payload(directory)};
        });
    }

    void begin_archive(
        const ArchiveRef& archive, const model_io::AgentInputState& original) override {
        map_failures([&] {
            const auto directory = require_archive(archive);
            fs::create_directories(archive_root());
            if (!fs::create_directory(directory)) {
                throw StoreError(
                    "compact archive directory already exists", Backend::Json,
                    CommitStatus::NotCommitted);
            }
            // A failed write keeps the reserved directory: the attempt consumed
            // its ordinal, and a payload renamed before a directory-sync failure
            // is still visible evidence. Retention does not recognize the
            // directory until it contains its one expected payload.
            load::save_state(
                archive_payload(directory), original, load::StateFormat::Readable);
        });
    }

    CommitReceipt commit_compaction(
        const ArchiveRef& archive, const model_io::AgentInputState& replacement,
        std::optional<Revision> expected_revision) override {
        (void)require_archive(archive);
        write_snapshot(replacement, expected_revision);
        return CommitReceipt{Backend::Json, CommitStatus::Committed, {}, std::nullopt};
    }

    void finish_archive_attempt(
        const ArchiveRef& archive, ArchiveOutcome outcome) override {
        (void)require_archive(archive);
        // The layout records a failed, cancelled or interrupted attempt
        // implicitly: the directory exists exactly when the attempt began and its
        // payload is never rewritten. Validate the vocabulary instead of writing
        // a marker that nothing would read.
        switch (outcome) {
            case ArchiveOutcome::Failed:
            case ArchiveOutcome::Cancelled:
            case ArchiveOutcome::Interrupted:
                return;
        }
        throw std::logic_error("invalid archive outcome");
    }

    CleanupResult prune_archives(const ArchivePruneRequest& request) override {
        return map_failures([&] {
            const auto current = require_archive(request.current);
            std::vector<fs::path> referenced;
            referenced.reserve(request.referenced.size());
            for (const auto& archive : request.referenced) {
                referenced.push_back(require_archive(archive));
            }
            const auto cleaned = load::prune_memory_archives(
                archive_root(), current, request.policy, referenced);
            return CleanupResult{cleaned.removed_archives, cleaned.removed_bytes};
        });
    }

    std::optional<ArchiveRef> archive_at(const fs::path& directory) const override {
        return map_failures([&]() -> std::optional<ArchiveRef> {
            if (directory.empty()) return std::nullopt;
            const auto normalized = fs::absolute(directory).lexically_normal();
            // Only direct children of this store's archive root can be protected
            // archives; a nested or foreign path is not evidence this store owns.
            if (normalized.parent_path() != archive_root()) return std::nullopt;
            return ArchiveRef{session_, normalized.filename().string(), Backend::Json,
                ArchiveKind::LegacyReadable, archive_payload(normalized)};
        });
    }

private:
    fs::path snapshot() const { return state_directory_ / "state.json"; }

    /**
     * Resolve the archive root on demand.
     *
     * Resolution is deferred so that constructing a store never reads the
     * working directory, and so a session without compact support keeps saving
     * state even though archive operations have nowhere to write.
     */
    fs::path archive_root() const {
        return fs::absolute(archive_root_).lexically_normal();
    }

    /** Archived payload inside one attempt directory; the layout stays here. */
    static fs::path archive_payload(const fs::path& directory) {
        return directory / "state.md";
    }

    /**
     * Validate a reference against this store and return its archive directory.
     *
     * Identifiers are backend-scoped, so a reference from another session or an
     * archive outside this root is a caller error rather than a cleanup target.
     * The payload name is derived from the directory, so a mismatched reference
     * is rejected instead of silently addressing a different file.
     */
    fs::path require_archive(const ArchiveRef& archive) const {
        if (archive.backend != Backend::Json || archive.session_id != session_) {
            throw std::invalid_argument("archive reference belongs to another store");
        }
        if (archive.payload.empty()) {
            throw std::invalid_argument("JSON archive reference requires a payload path");
        }
        const auto payload = fs::absolute(archive.payload).lexically_normal();
        const auto directory = payload.parent_path();
        if (directory.parent_path() != archive_root()
            || payload.filename() != archive_payload(directory).filename()) {
            throw std::invalid_argument("archive reference is not a JSON compact archive");
        }
        return directory;
    }

    /** Publish the authoritative snapshot, mapping publication state to recovery. */
    void write_snapshot(
        const model_io::AgentInputState& state,
        const std::optional<Revision>& expected_revision) const {
        if (expected_revision) {
            throw std::invalid_argument(
                "JSON persistence cannot verify an expected revision");
        }
        map_failures([&] {
            load::save_state(snapshot(), state, load::StateFormat::Json);
        });
    }

    std::string session_;
    fs::path state_directory_;
    fs::path archive_root_;
};

} // namespace

std::unique_ptr<StateStore> make_json_store(
    std::string session_id, JsonStoreOptions options) {
    return std::make_unique<JsonStore>(std::move(session_id), std::move(options));
}

} // namespace persistence
