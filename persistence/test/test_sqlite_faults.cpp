#define BOOST_TEST_MODULE SqliteCheckpointFaults
#include <boost/test/unit_test.hpp>
#include "persistence/store.hpp"
#include <sqlite3.h>
#include <filesystem>
#include <map>
#include <unistd.h>

namespace {
/** Test-only VFS shim: inject actual SQLite write/sync failures without adding
 * fault switches to the production API. Connections are closed before teardown.
 */
struct Faults {
    struct File {
        sqlite3_io_methods methods;
        const sqlite3_io_methods* original;
    };
    static inline std::map<sqlite3_file*, File> files;
    static inline int error = SQLITE_OK;
    static inline bool sync = false;
    static inline int hits = 0;
    static inline decltype(sqlite3_vfs::xOpen) original_open;
    sqlite3_vfs* vfs = sqlite3_vfs_find(nullptr);

    Faults() {
        original_open = vfs->xOpen;
        vfs->xOpen = open;
    }
    ~Faults() {
        error = SQLITE_OK;
        vfs->xOpen = original_open;
    }
    static int close(sqlite3_file* file) {
        auto original = files.at(file).original;
        file->pMethods = original;
        files.erase(file);
        return original->xClose(file);
    }
    static int write(sqlite3_file* file, const void* data, int size, sqlite3_int64 offset) {
        if (!sync && error != SQLITE_OK) { ++hits; return error; }
        return files.at(file).original->xWrite(file, data, size, offset);
    }
    static int synchronize(sqlite3_file* file, int flags) {
        if (sync && error != SQLITE_OK) { ++hits; return error; }
        return files.at(file).original->xSync(file, flags);
    }
    static int open(sqlite3_vfs* vfs, const char* name, sqlite3_file* file, int flags, int* output) {
        const int result = original_open(vfs, name, file, flags, output);
        if (result != SQLITE_OK || !file->pMethods) return result;
        auto& entry = files[file];
        entry.original = file->pMethods;
        entry.methods = *file->pMethods;
        entry.methods.xClose = close;
        entry.methods.xWrite = write;
        entry.methods.xSync = synchronize;
        file->pMethods = &entry.methods;
        return result;
    }
};
}

BOOST_AUTO_TEST_CASE(full_and_ioerr_never_publish_partial_state) {
    const auto root = std::filesystem::temp_directory_path()
        / ("simplex_sqlite_faults_" + std::to_string(getpid()));
    for (const auto error : {SQLITE_FULL, SQLITE_IOERR_WRITE, SQLITE_IOERR_FSYNC}) {
        std::filesystem::remove_all(root);
        auto store = persistence::make_sqlite_store("test", root);
        model_io::AgentInputState before;
        before.meta.session_id = "test";
        before.extras = nlohmann::json{{"before", std::string(100000, 'a')}};
        const auto receipt = store->checkpoint(before, persistence::Boundary::BeforeTools);
        // Keep a second connection open so the fault targets the next transaction,
        // not normal last-connection WAL deletion/recreation.
        sqlite3* reader = nullptr;
        BOOST_REQUIRE(sqlite3_open((root / "state.sqlite3").c_str(), &reader) == SQLITE_OK);
        BOOST_REQUIRE(sqlite3_exec(reader, "SELECT * FROM snapshots", nullptr, nullptr, nullptr) == SQLITE_OK);
        auto after = before;
        after.extras = nlohmann::json{{"after", std::string(200000, 'b')}};
        std::string attempted_operation;
        {
            Faults faults;
            Faults::error = error;
            Faults::sync = error == SQLITE_IOERR_FSYNC;
            Faults::hits = 0;
            bool uncertain = false;
            try {
                store->checkpoint(after, persistence::Boundary::RunFinished);
                BOOST_FAIL("injected SQLite failure was not propagated");
            } catch (const persistence::StoreError& failure) {
                attempted_operation = failure.operation_id();
                uncertain = failure.status() == persistence::CommitStatus::Unknown;
            }
            BOOST_TEST(Faults::hits > 0);
            Faults::error = SQLITE_OK;
            if (uncertain) {
                BOOST_CHECK_THROW(store->checkpoint(after, persistence::Boundary::RunFinished), persistence::StoreError);
            }
        }
        BOOST_REQUIRE(sqlite3_close(reader) == SQLITE_OK);
        auto reopened = persistence::make_sqlite_store("test", root);
        auto loaded = reopened->load_current();
        BOOST_REQUIRE(loaded);
        const auto json = nlohmann::json(loaded->state);
        BOOST_CHECK(json == nlohmann::json(before) || json == nlohmann::json(after));
        if (json == nlohmann::json(before)) BOOST_TEST(loaded->operation_id == receipt.operation_id);
        else {
            BOOST_TEST(*loaded->revision == 2u);
            BOOST_TEST(loaded->operation_id == attempted_operation);
        }
    }
    std::filesystem::remove_all(root);
}
