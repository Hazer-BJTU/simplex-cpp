#include "persistence/store.hpp"

#include "sqlite_schema.hpp"
#include "sqlite_snapshot.hpp"
#include "sqlite/database.hpp"

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <limits>
#include <system_error>
#include <unordered_map>
#include <set>

namespace persistence {
namespace {

/** Small descriptor guard for private creation and directory durability. */
class Descriptor {
public:
    explicit Descriptor(int value) : value_(value) {
        if (value < 0) throw std::system_error(errno, std::generic_category(), "SQLite file operation");
    }
    ~Descriptor() { ::close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return value_; }
private:
    int value_;
};

/** A fresh connection per operation avoids retaining a failed transaction. */
class SqliteStore final : public StateStore {
public:
    SqliteStore(std::string session, std::filesystem::path directory)
        : session_(std::move(session)), path_(std::move(directory) / "state.sqlite3") {}

    Backend backend() const noexcept override { return Backend::Sqlite; }
    const std::string& session_id() const noexcept override { return session_; }

    std::optional<StoredState> load_current() override {
        try {
            if (!std::filesystem::exists(path_)) {
                require_new_sqlite_directory();
                return std::nullopt;
            }
            sqlite::Database database(path_.string(), sqlite::OpenMode::ReadOnly);
            sqlite_schema::inspect_store(database, session_);
            sqlite::Transaction transaction(database);
            auto state = sqlite_snapshot::load_current(database, session_, false);
            if (!state) return std::nullopt;
            const auto metadata = current_metadata(database);
            return StoredState{std::move(*state), backend(), metadata.first, metadata.second};
        } catch (const std::exception& error) {
            throw StoreError(error.what(), backend(), CommitStatus::NotCommitted);
        }
    }

    CommitReceipt checkpoint(const model_io::AgentInputState& state, Boundary boundary,
                             std::optional<Revision> expected) override {
        (void)boundary_name(boundary);
        if (state.meta.session_id != session_) {
            throw StoreError("SQLite checkpoint session mismatch", backend(), CommitStatus::NotCommitted);
        }
        return write("checkpoint", [&](sqlite::Database& database, const std::string& operation) {
            return replace(database, state, expected, operation);
        });
    }

    ArchiveRef reserve_archive(const CompactAttempt& attempt) override {
        if (attempt.run_id.empty()) {
            throw std::invalid_argument("compact requires a run identity");
        }
        // Reservation does no I/O. Random identities cannot be reused across
        // restarts, failed preflights or retention; SQL sequence orders T1 commits.
        const auto id = boost::uuids::to_string(boost::uuids::random_generator()());
        reservations_.emplace(id, attempt);
        return {session_, id, backend(), ArchiveKind::SqliteSnapshot, {}};
    }

    CommitReceipt begin_archive(const ArchiveRef& archive,
                                const model_io::AgentInputState& original,
                                std::optional<Revision> expected) override {
        validate(archive);
        if (original.meta.session_id != session_) {
            throw StoreError("SQLite archive session mismatch", backend(), CommitStatus::NotCommitted);
        }
        const auto found = reservations_.find(archive.archive_id);
        if (found == reservations_.end()) {
            throw std::invalid_argument("SQLite archive was not reserved by this store");
        }
        const auto attempt = found->second;
        reservations_.erase(found); // Failed T1 never reuses its reservation.
        return write("archive preparation", [&](sqlite::Database& database, const std::string& operation) {
            // T1 is also a mandatory original-state checkpoint: ordinary save
            // switches must not leave restart with an older conversation.
            const auto revision = replace(database, original, expected, operation);
            const auto snapshot = sqlite_snapshot::store_archive_snapshot(
                database, session_, original, false);
            auto metadata = database.prepare(
                "UPDATE snapshots SET revision = ?1, operation_id = ?2 WHERE snapshot_id = ?3");
            metadata.bind_int64(1, static_cast<std::int64_t>(revision));
            metadata.bind_text(2, operation);
            metadata.bind_int64(3, snapshot);
            metadata.step();
            auto sequence = database.prepare("SELECT COALESCE(MAX(sequence), 0) FROM archives");
            sequence.step();
            const auto previous = sequence.column_int64(0);
            if (previous == std::numeric_limits<std::int64_t>::max()) {
                throw std::overflow_error("SQLite archive sequence exhausted");
            }
            auto insert = database.prepare(
                "INSERT INTO archives (archive_id, session_id, archive_kind, sequence, "
                "storage_kind, original_snapshot_id, mode, run_id, call_id, outcome, created_at) "
                "VALUES (?1, ?2, 'snapshot', ?3, 'sqlite', ?4, ?5, ?6, ?7, 'prepared', ?8)");
            insert.bind_text(1, archive.archive_id);
            insert.bind_text(2, session_);
            insert.bind_int64(3, previous + 1);
            insert.bind_int64(4, snapshot);
            insert.bind_text(5, attempt.automatic ? "automatic" : "manual");
            insert.bind_text(6, attempt.run_id);
            if (attempt.call_id.empty()) insert.bind_null(7);
            else insert.bind_text(7, attempt.call_id);
            insert.bind_text(8, utc_timestamp());
            insert.step();
            return revision;
        });
    }

    CommitReceipt commit_compaction(const ArchiveRef& archive,
                                     const model_io::AgentInputState& replacement,
                                     std::optional<Revision> expected) override {
        validate(archive);
        return write("compaction", [&](sqlite::Database& database, const std::string& operation) {
            // The original snapshot records the revision observed at T1 even
            // when ordinary saves were disabled. T2 cannot replace later work.
            auto prepared = database.prepare(
                "SELECT s.revision FROM archives a JOIN snapshots s "
                "ON s.snapshot_id = a.original_snapshot_id "
                "WHERE a.archive_id = ?1 AND a.session_id = ?2 "
                "AND a.outcome = 'prepared' AND a.storage_kind = 'sqlite' "
                "AND s.session_id = ?2 AND s.kind = 'archive'");
            prepared.bind_text(1, archive.archive_id);
            prepared.bind_text(2, session_);
            if (prepared.step() != sqlite::StepResult::Row) {
                throw std::runtime_error("SQLite compact attempt is not prepared");
            }
            const auto base = static_cast<Revision>(prepared.column_int64(0));
            if (expected && *expected != base) {
                throw std::runtime_error("stale SQLite compact revision");
            }
            const auto revision = replace(database, replacement, base, operation);
            auto settle = database.prepare(
                "UPDATE archives SET outcome = 'succeeded', replacement_revision = ?1 "
                "WHERE archive_id = ?2");
            settle.bind_int64(1, static_cast<std::int64_t>(revision));
            settle.bind_text(2, archive.archive_id);
            settle.step();
            return revision;
        });
    }

    void finish_archive_attempt(const ArchiveRef& archive, ArchiveOutcome outcome) override {
        validate(archive);
        const char* label = nullptr;
        switch (outcome) {
        case ArchiveOutcome::Failed: label = "failed"; break;
        case ArchiveOutcome::Cancelled: label = "cancelled"; break;
        case ArchiveOutcome::Interrupted: label = "interrupted"; break;
        default: throw std::invalid_argument("invalid archive outcome");
        }
        write("archive settlement", [&](sqlite::Database& database, const std::string&) {
            require_archive(database, archive);
            // Never overwrite a succeeded/interrupted attempt during unwinding.
            auto update = database.prepare(
                "UPDATE archives SET outcome = ?1 WHERE archive_id = ?2 AND outcome = 'prepared'");
            update.bind_text(1, label);
            update.bind_text(2, archive.archive_id);
            update.step();
            return current_metadata(database).first;
        });
    }

    CleanupResult prune_archives(const ArchivePruneRequest& request) override {
        validate(request.current);
        std::set<std::string> protected_ids{request.current.archive_id};
        for (const auto& reference : request.referenced) {
            validate(reference);
            protected_ids.insert(reference.archive_id);
        }
        CleanupResult result;
        write("archive retention", [&](sqlite::Database& database, const std::string&) {
            require_archive(database, request.current);
            if (request.policy.max_archives == 0) return current_metadata(database).first;
            struct Candidate { std::string id; std::int64_t snapshot; bool protected_archive; };
            std::vector<Candidate> candidates;
            auto query = database.prepare(
                "SELECT a.archive_id, a.original_snapshot_id, a.outcome FROM archives a "
                "JOIN snapshots s ON s.snapshot_id = a.original_snapshot_id "
                "WHERE a.session_id = ?1 AND a.storage_kind = 'sqlite' "
                "AND s.session_id = ?1 AND s.kind = 'archive' ORDER BY a.sequence DESC");
            query.bind_text(1, session_);
            std::size_t kept = 0;
            while (query.step() == sqlite::StepResult::Row) {
                const auto id = query.column_text(0);
                // An unfinished/recovered attempt remains evidence until a later
                // operator decision. Do not turn a crash into automatic deletion.
                const auto outcome = query.column_text(2);
                const bool protect = protected_ids.contains(id)
                    || outcome == "prepared" || outcome == "interrupted";
                if (protect) ++kept;
                candidates.push_back({id, query.column_int64(1), protect});
            }
            for (const auto& candidate : candidates) {
                if (candidate.protected_archive) continue;
                if (kept < request.policy.max_archives) { ++kept; continue; }
                auto remove = database.prepare("DELETE FROM archives WHERE archive_id = ?1");
                remove.bind_text(1, candidate.id);
                remove.step();
                auto graph = database.prepare("DELETE FROM snapshots WHERE snapshot_id = ?1");
                graph.bind_int64(1, candidate.snapshot);
                graph.step();
                ++result.removed_archives;
            }
            // removed_bytes is zero: released database pages remain reusable,
            // and this operation does not promise to shrink the physical file.
            return current_metadata(database).first;
        });
        return result;
    }

    std::optional<ArchiveRef> archive_at(const std::filesystem::path&) const override {
        return std::nullopt;
    }

private:
    /** Backend selection is not migration: never silently start over old JSON. */
    void require_new_sqlite_directory() const {
        if (std::filesystem::exists(path_.parent_path() / "state.json")) {
            throw std::runtime_error("existing JSON state requires migration before selecting SQLite");
        }
    }

    /** Read metadata inside the same transaction as its state graph. */
    std::pair<Revision, std::string> current_metadata(sqlite::Database& database) const {
        auto query = database.prepare(
            "SELECT revision, operation_id FROM snapshots WHERE session_id = ?1 AND kind = 'current'");
        query.bind_text(1, session_);
        if (query.step() == sqlite::StepResult::Done) return {0, {}};
        const auto revision = query.column_int64(0);
        if (revision < 0) throw std::runtime_error("invalid SQLite revision");
        return {static_cast<Revision>(revision), query.column_text(1)};
    }

    /** Validate backend identity before any disk access. */
    void validate(const ArchiveRef& archive) const {
        if (archive.session_id != session_ || archive.backend != backend()
            || archive.kind != ArchiveKind::SqliteSnapshot || archive.archive_id.empty()
            || !archive.payload.empty()) {
            throw std::invalid_argument("foreign SQLite archive reference");
        }
    }

    void require_archive(sqlite::Database& database, const ArchiveRef& archive) const {
        auto query = database.prepare(
            "SELECT 1 FROM archives WHERE archive_id = ?1 AND session_id = ?2 AND storage_kind = 'sqlite'");
        query.bind_text(1, archive.archive_id);
        query.bind_text(2, session_);
        if (query.step() != sqlite::StepResult::Row) {
            throw std::runtime_error("SQLite archive does not exist");
        }
    }

    /** Replace current state inside the caller's transaction, never the archive. */
    Revision replace(sqlite::Database& database, const model_io::AgentInputState& state,
                     std::optional<Revision> expected, const std::string& operation) {
        const auto previous = current_metadata(database).first;
        if (expected && *expected != previous) {
            throw std::runtime_error("stale SQLite checkpoint revision");
        }
        if (previous >= static_cast<Revision>(std::numeric_limits<std::int64_t>::max())) {
            throw std::overflow_error("SQLite revision exhausted");
        }
        const auto snapshot = sqlite_snapshot::replace_current(database, session_, state, false);
        auto update = database.prepare(
            "UPDATE snapshots SET revision = ?1, operation_id = ?2 WHERE snapshot_id = ?3");
        update.bind_int64(1, static_cast<std::int64_t>(previous + 1));
        update.bind_text(2, operation);
        update.bind_int64(3, snapshot);
        update.step();
        return previous + 1;
    }

    /** One short durable mutation. No connection or transaction survives a call.
     * A first writer reconciles abandoned prepared attempts in the same commit;
     * load_current remains strictly read-only. Unknown commits latch every write,
     * including optional settlement/retention, to preserve both recovery candidates.
     */
    template<class Mutation>
    CommitReceipt write(const char* action, Mutation mutation) {
        if (uncertain_) {
            throw StoreError("reopen SQLite store after an uncertain commit", backend(), CommitStatus::Unknown);
        }
        const auto operation = boost::uuids::to_string(boost::uuids::random_generator()());
        bool committing = false;
        try {
            std::filesystem::create_directories(path_.parent_path());
            if (!std::filesystem::exists(path_)) require_new_sqlite_directory();
            bool created = false;
            const int descriptor = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (descriptor >= 0) {
                Descriptor file(descriptor);
                created = true;
            } else if (errno != EEXIST) {
                throw std::system_error(errno, std::generic_category(), "create SQLite store");
            }
            sqlite::Database database(path_.string());
            if (!created) sqlite_schema::inspect_store(database, session_);
            sqlite_schema::open_store(database, session_, utc_timestamp());
            sqlite::Transaction transaction(database);
            if (!reconciled_) {
                database.execute("UPDATE archives SET outcome = 'interrupted' WHERE outcome = 'prepared'");
            }
            const auto revision = mutation(database, operation);
            committing = true;
            transaction.commit();
            Descriptor directory(::open(path_.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            if (::fsync(directory.get()) != 0) {
                throw std::system_error(errno, std::generic_category(), "sync SQLite directory");
            }
            reconciled_ = true;
            return {backend(), CommitStatus::Committed, operation, revision};
        } catch (const std::exception& error) {
            uncertain_ = committing;
            throw StoreError(std::string("SQLite ") + action + " " + operation
                + (committing ? " (unknown): " : " (not_committed): ") + error.what(),
                backend(), committing ? CommitStatus::Unknown : CommitStatus::NotCommitted, operation);
        }
    }

    std::string session_;
    std::filesystem::path path_;
    bool uncertain_ = false;
    bool reconciled_ = false;
    std::unordered_map<std::string, CompactAttempt> reservations_;
};
}

std::unique_ptr<StateStore> make_sqlite_store(std::string session_id,
                                             std::filesystem::path state_directory) {
    return std::make_unique<SqliteStore>(std::move(session_id), std::move(state_directory));
}
}
