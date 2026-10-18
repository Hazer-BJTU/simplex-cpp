#include "persistence/store.hpp"

#include "fileio/atomic_write.hpp"
#include "json_archive.hpp"
#include "load/persistence.hpp"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace persistence {
namespace {

namespace fs = std::filesystem;

/**
 * Translate backend failures into the store contract.
 *
 * Codec, filesystem and publication failures become StoreError so that callers
 * can rely on status() for recovery, including the published-but-unsynced case
 * that both the codec and the manifest writer report through published().
 * Programming errors such as an invalid argument are deliberately not converted:
 * they are caller bugs, not storage outcomes, and must not be reported as a
 * failed write.
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
    } catch (const fileio::AtomicWriteError& error) {
        throw StoreError(error.what(), Backend::Json,
            error.published() ? CommitStatus::Unknown : CommitStatus::NotCommitted);
    } catch (const fs::filesystem_error& error) {
        throw StoreError(error.what(), Backend::Json, CommitStatus::NotCommitted);
    }
}

/**
 * Atomic JSON snapshots with complete JSON compact archives.
 *
 * The adapter publishes `state/state.json` through the JSON codec and freezes
 * each compact attempt as `<memory>/<archive-id>/{state.json,manifest.json}`.
 * The manifest is the attempt's publication marker and identity record; see
 * json_archive.hpp for the layout and recognition rules.
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
            const auto directory = json_archive::plan_directory(archive_root(), attempt.run_id);
            return ArchiveRef{session_, directory.filename().string(), Backend::Json,
                ArchiveKind::JsonState, directory / json_archive::state_file};
        });
    }

    CommitReceipt begin_archive(
        const ArchiveRef& archive, const model_io::AgentInputState& original,
        std::optional<Revision> expected_revision) override {
        if (expected_revision) {
            throw std::invalid_argument("JSON archives do not support revision expectations");
        }
        map_failures([&] {
            const auto directory = require_restorable(archive);
            fs::create_directories(archive_root());
            if (!fs::create_directory(directory)) {
                throw StoreError(
                    "compact archive directory already exists", Backend::Json,
                    CommitStatus::NotCommitted);
            }
            // A failed publication keeps the reserved directory: the attempt
            // consumed its ordinal, and a payload renamed into place before a
            // directory-sync failure is still visible evidence. Without its
            // manifest the archive stays incomplete, so retention ignores it and
            // nothing mistakes it for restorable state.
            json_archive::publish(directory, session_, original);
        });
        return {Backend::Json, CommitStatus::Committed, {}, std::nullopt};
    }

    CommitReceipt commit_compaction(
        const ArchiveRef& archive, const model_io::AgentInputState& replacement,
        std::optional<Revision> expected_revision) override {
        (void)require_restorable(archive);
        write_snapshot(replacement, expected_revision);
        return CommitReceipt{Backend::Json, CommitStatus::Committed, {}, std::nullopt};
    }

    void finish_archive_attempt(
        const ArchiveRef& archive, ArchiveOutcome outcome) override {
        (void)require_restorable(archive);
        // The layout records a failed, cancelled or interrupted attempt
        // implicitly: the archive is exactly the state frozen before
        // summarization, and the authoritative snapshot still holds the original
        // conversation. Writing an outcome marker would add a fallible write
        // after a durable commit, so validation replaces bookkeeping.
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
            return json_archive::prune(archive_root(), session_, request.current,
                request.referenced, request.policy);
        });
    }

    std::optional<ArchiveRef> archive_at(const fs::path& directory) const override {
        if (archive_root_.empty()) return std::nullopt;
        return map_failures([&] {
            return json_archive::reference_at(archive_root(), directory, session_);
        });
    }

private:
    fs::path snapshot() const { return state_directory_ / "state.json"; }

    /**
     * Resolve the archive root on demand.
     *
     * Resolution is deferred so that constructing a store never reads the
     * working directory, and a session without compact support still saves
     * state. A store without an archive root cannot archive, and says so instead
     * of writing attempts below the working directory.
     */
    fs::path archive_root() const {
        if (archive_root_.empty()) {
            throw StoreError(
                "compact archive root is not configured", Backend::Json,
                CommitStatus::NotCommitted);
        }
        return fs::absolute(archive_root_).lexically_normal();
    }

    /**
     * Validate a reference this store may publish or settle.
     *
     * Identifiers are backend-scoped, so a reference from another session, a
     * legacy Markdown artifact or an archive outside this root is a caller error
     * rather than a cleanup target. The payload name and directory are derived
     * from the reference, so a mismatched record is rejected instead of silently
     * addressing a different file.
     */
    fs::path require_restorable(const ArchiveRef& archive) const {
        if (archive.backend != Backend::Json || archive.session_id != session_) {
            throw std::invalid_argument("archive reference belongs to another store");
        }
        if (archive.kind != ArchiveKind::JsonState) {
            throw std::invalid_argument("only a JSON state archive can be published");
        }
        if (archive.payload.empty()) {
            throw std::invalid_argument("JSON archive reference requires a payload path");
        }
        const auto payload = fs::absolute(archive.payload).lexically_normal();
        const auto directory = payload.parent_path();
        if (payload.filename() != json_archive::state_file
            || directory.parent_path() != archive_root()) {
            throw std::invalid_argument("archive reference is not a JSON compact archive");
        }
        if (archive.archive_id != directory.filename().string()
            || !json_archive::is_archive_name(archive.archive_id)) {
            throw std::invalid_argument("archive reference has an invalid identity");
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
