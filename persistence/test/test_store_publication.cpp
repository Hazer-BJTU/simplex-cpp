#define BOOST_TEST_MODULE PersistenceStorePublication
#include <boost/test/unit_test.hpp>

#include "persistence/store.hpp"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

/** Interpose fsync only in this executable; production code has no test seam. */
enum class Failure {
    None,
    FileSync,
    DirectorySync,
    /// Let the payload sync succeed, then fail the manifest's file sync.
    ManifestFileSync,
};

Failure failure = Failure::None;
/// Remaining file syncs while Failure::ManifestFileSync is armed.
int file_syncs_left = 0;

/** Restore normal IO even when a test assertion or operation throws. */
struct InjectFailure {
    explicit InjectFailure(Failure point) {
        failure = point;
        file_syncs_left = point == Failure::ManifestFileSync ? 2 : 0;
    }
    ~InjectFailure() {
        failure = Failure::None;
        file_syncs_left = 0;
    }
};

struct Scratch {
    fs::path root = fs::temp_directory_path()
        / ("simplex_store_publication_" + std::to_string(::getpid()));

    Scratch() {
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    fs::path state() const { return root / "session/state"; }
    fs::path memory() const { return root / "session/memory"; }
    fs::path snapshot() const { return state() / "state.json"; }
};

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

/** Compare store enums in assertions without adding stream operators to the API. */
template<typename Enum>
int code(Enum value) {
    return static_cast<int>(value);
}

model_io::AgentInputState example_state(std::string updated) {
    model_io::AgentInputState state;
    state.meta.session_id = "session-1";
    state.meta.updated_at = std::move(updated);
    state.system_prompt.add_section("system", "Instructions", "Be precise.");
    return state;
}

} // namespace

/** Exported via ENABLE_EXPORTS so shared-library calls reach the injector. */
extern "C" int fsync(int descriptor) {
    struct stat status {};
    if (::fstat(descriptor, &status) == 0) {
        const bool directory = S_ISDIR(status.st_mode);
        if (!directory && failure == Failure::ManifestFileSync) {
            if (--file_syncs_left == 0) {
                errno = EIO;
                return -1;
            }
        } else if ((directory && failure == Failure::DirectorySync)
                   || (!directory && failure == Failure::FileSync)) {
            errno = EIO;
            return -1;
        }
    }
    return static_cast<int>(::syscall(SYS_fsync, descriptor));
}

BOOST_AUTO_TEST_CASE(checkpoint_reports_publication_state_instead_of_failing_blindly) {
    Scratch scratch;
    auto store = persistence::make_json_store("session-1", {scratch.state(), scratch.memory()});
    BOOST_REQUIRE(code(store->checkpoint(example_state("first"),
        persistence::Boundary::BeforeTools).status)
        == code(persistence::CommitStatus::Committed));
    const auto previous = read_file(scratch.snapshot());

    for (const auto point : {Failure::FileSync, Failure::DirectorySync}) {
        const bool published = point == Failure::DirectorySync;
        {
            InjectFailure inject(point);
            BOOST_CHECK_EXCEPTION(
                store->checkpoint(example_state("second"), persistence::Boundary::StepFinished),
                persistence::StoreError,
                [published](const persistence::StoreError& error) {
                    // A rename that outlived a failed directory sync is Unknown,
                    // never a clean NotCommitted: the caller must inspect it.
                    return error.status() == (published
                            ? persistence::CommitStatus::Unknown
                            : persistence::CommitStatus::NotCommitted)
                        && error.backend() == persistence::Backend::Json
                        && std::string(error.what()).find("state.json") != std::string::npos;
                });
        }
        const auto current = read_file(scratch.snapshot());
        BOOST_TEST((current != previous) == published);
        // The store still decodes whatever publication left behind.
        const auto restored = store->load_current();
        BOOST_REQUIRE(restored.has_value());
        BOOST_TEST(restored->state.meta.updated_at == (published ? "second" : "first"));
        for (const auto& entry : fs::directory_iterator(scratch.state())) {
            BOOST_TEST(!entry.path().filename().string().starts_with(".simplex-write-"));
        }
    }
}

BOOST_AUTO_TEST_CASE(begin_archive_reports_publication_and_keeps_its_ordinal) {
    Scratch scratch;
    auto store = persistence::make_json_store("session-1", {scratch.state(), scratch.memory()});
    const auto original = example_state("first");
    const auto first = store->reserve_archive({"11111111-1111-4111-8111-000000000001"});
    BOOST_TEST(first.archive_id.starts_with("00000000000000000001-"));

    // A file-sync failure publishes nothing, but the reserved attempt keeps its
    // identity: ordinals are never reused, so sequence order stays meaningful.
    {
        InjectFailure inject(Failure::FileSync);
        BOOST_CHECK_EXCEPTION(
            store->begin_archive(first, original),
            persistence::StoreError,
            [](const persistence::StoreError& error) {
                return error.status() == persistence::CommitStatus::NotCommitted;
            });
    }
    BOOST_TEST(!fs::exists(first.payload));
    BOOST_TEST(fs::exists(first.payload.parent_path()));

    // A rename that outlived a failed directory sync leaves a visible payload and
    // must be reported as Unknown instead of a clean failure. Without its
    // manifest the attempt is incomplete: no user and no retention pass may
    // mistake it for restorable state.
    const auto second = store->reserve_archive({"11111111-1111-4111-8111-000000000002"});
    BOOST_TEST(second.archive_id.starts_with("00000000000000000002-"));
    {
        InjectFailure inject(Failure::DirectorySync);
        BOOST_CHECK_EXCEPTION(
            store->begin_archive(second, original),
            persistence::StoreError,
            [](const persistence::StoreError& error) {
                return error.status() == persistence::CommitStatus::Unknown;
            });
    }
    BOOST_TEST(fs::exists(second.payload));
    BOOST_TEST(!read_file(second.payload).empty());
    BOOST_TEST(!fs::exists(second.payload.parent_path() / "manifest.json"));
    BOOST_TEST(!store->archive_at(second.payload).has_value());

    // The manifest is the publication point: failing its own file sync publishes
    // no marker, so the payload stays incomplete evidence as well.
    const auto third = store->reserve_archive({"11111111-1111-4111-8111-000000000003"});
    BOOST_TEST(third.archive_id.starts_with("00000000000000000003-"));
    {
        InjectFailure inject(Failure::ManifestFileSync);
        BOOST_CHECK_EXCEPTION(
            store->begin_archive(third, original),
            persistence::StoreError,
            [](const persistence::StoreError& error) {
                return error.status() == persistence::CommitStatus::NotCommitted;
            });
    }
    BOOST_TEST(fs::exists(third.payload));
    BOOST_TEST(!fs::exists(third.payload.parent_path() / "manifest.json"));
    BOOST_TEST(!store->archive_at(third.payload).has_value());

    const auto fourth = store->reserve_archive({"11111111-1111-4111-8111-000000000004"});
    BOOST_TEST(fourth.archive_id.starts_with("00000000000000000004-"));
}

BOOST_AUTO_TEST_CASE(commit_compaction_uses_the_same_publication_contract) {
    Scratch scratch;
    auto store = persistence::make_json_store("session-1", {scratch.state(), scratch.memory()});
    const auto original = example_state("first");
    BOOST_REQUIRE(code(store->checkpoint(original,
        persistence::Boundary::BeforeTools).status)
        == code(persistence::CommitStatus::Committed));
    const auto archive = store->reserve_archive({"11111111-1111-4111-8111-000000000010"});
    store->begin_archive(archive, original);
    const auto frozen = read_file(archive.payload);

    {
        InjectFailure inject(Failure::DirectorySync);
        BOOST_CHECK_EXCEPTION(
            store->commit_compaction(archive, example_state("second")),
            persistence::StoreError,
            [](const persistence::StoreError& error) {
                return error.status() == persistence::CommitStatus::Unknown;
            });
    }
    BOOST_TEST(read_file(archive.payload) == frozen);
    BOOST_TEST(store->load_current()->state.meta.updated_at == "second");
}
