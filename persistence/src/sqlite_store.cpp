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
        if (uncertain_) throw StoreError("reopen SQLite store after an uncertain commit", backend(), CommitStatus::Unknown);
        const auto operation = boost::uuids::to_string(boost::uuids::random_generator()());
        bool committing = false;
        try {
            if (state.meta.session_id != session_) {
                throw std::invalid_argument("SQLite checkpoint session mismatch");
            }
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
            // Only our exclusively created file may acquire a schema. Existing
            // empty, foreign or damaged files must never become fresh sessions.
            if (!created) sqlite_schema::inspect_store(database, session_);
            sqlite_schema::open_store(database, session_, utc_timestamp());
            sqlite::Transaction transaction(database);
            const auto previous = current_metadata(database).first;
            if (expected && *expected != previous) {
                throw std::runtime_error("stale SQLite checkpoint revision");
            }
            if (previous >= static_cast<Revision>(std::numeric_limits<std::int64_t>::max())) {
                throw std::overflow_error("SQLite revision exhausted");
            }
            const auto revision = previous + 1;
            const auto snapshot = sqlite_snapshot::replace_current(database, session_, state, false);
            auto update = database.prepare(
                "UPDATE snapshots SET revision = ?1, operation_id = ?2 WHERE snapshot_id = ?3");
            update.bind_int64(1, static_cast<std::int64_t>(revision));
            update.bind_text(2, operation);
            update.bind_int64(3, snapshot);
            update.step();
            committing = true;
            transaction.commit();
            // FULL protects SQLite's WAL; sync its containing directory as well
            // so a newly created store's name survives a system crash.
            Descriptor directory(::open(path_.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            if (::fsync(directory.get()) != 0) {
                throw std::system_error(errno, std::generic_category(), "sync SQLite directory");
            }
            return {backend(), CommitStatus::Committed, operation, revision};
        } catch (const std::exception& error) {
            // COMMIT errors are conservatively uncertain, even when SQLite
            // rolled back. Never retry blindly; reopen and inspect operation_id.
            uncertain_ = committing;
            // Keep correlation available even when a top-level exception logger
            // retains only what(); no SQL or state values enter this diagnostic.
            const auto outcome = committing ? "unknown" : "not_committed";
            throw StoreError("SQLite checkpoint " + operation + " (" + outcome + "): "
                + error.what(), backend(), committing ? CommitStatus::Unknown
                : CommitStatus::NotCommitted, operation);
        }
    }

    ArchiveRef reserve_archive(const CompactAttempt&) override { unsupported(); }
    void begin_archive(const ArchiveRef&, const model_io::AgentInputState&) override { unsupported(); }
    CommitReceipt commit_compaction(const ArchiveRef&, const model_io::AgentInputState&,
                                     std::optional<Revision>) override { unsupported(); }
    void finish_archive_attempt(const ArchiveRef&, ArchiveOutcome) override { unsupported(); }
    CleanupResult prune_archives(const ArchivePruneRequest&) override { unsupported(); }
    std::optional<ArchiveRef> archive_at(const std::filesystem::path&) const override { return std::nullopt; }

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

    [[noreturn]] void unsupported() const {
        throw StoreError("experimental SQLite does not support compact", backend(), CommitStatus::NotCommitted);
    }

    std::string session_;
    std::filesystem::path path_;
    bool uncertain_ = false;
};
}

std::unique_ptr<StateStore> make_sqlite_store(std::string session_id,
                                             std::filesystem::path state_directory) {
    return std::make_unique<SqliteStore>(std::move(session_id), std::move(state_directory));
}
}
