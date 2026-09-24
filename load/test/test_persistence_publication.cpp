#define BOOST_TEST_MODULE PersistencePublication
#include <boost/test/unit_test.hpp>

#include "fileio/atomic_write.hpp"
#include "load/persistence.hpp"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

/** Interpose fsync only in this executable; production code has no test seam. */
enum class Failure {
    None,
    FileSync,
    DirectorySync,
};

Failure failure = Failure::None;

/** Restore normal IO even when a test assertion or operation throws. */
struct InjectFailure {
    explicit InjectFailure(Failure value) { failure = value; }
    ~InjectFailure() { failure = Failure::None; }
};

struct Scratch {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / ("simplex_publication_" + std::to_string(::getpid()));

    Scratch() {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
    }

    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    std::string read() const {
        std::ifstream input(root / "state");
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    void no_temporary_files() const {
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            BOOST_TEST(!entry.path().filename().string().starts_with(".simplex-write-"));
        }
    }
};

} // namespace

/** Exported via ENABLE_EXPORTS so shared-library calls reach the injector. */
extern "C" int fsync(int descriptor) {
    struct stat status {};
    if (::fstat(descriptor, &status) == 0) {
        const bool directory = S_ISDIR(status.st_mode);
        if ((directory && failure == Failure::DirectorySync)
            || (!directory && failure == Failure::FileSync)) {
            errno = EIO;
            return -1;
        }
    }
    return static_cast<int>(::syscall(SYS_fsync, descriptor));
}

BOOST_AUTO_TEST_CASE(atomic_write_reports_publication_and_preserves_errno) {
    Scratch scratch;
    for (const auto point : {Failure::FileSync, Failure::DirectorySync}) {
        fileio::atomic_write(scratch.root / "state", [](std::ostream& output) {
            output << "old";
        });
        const bool published = point == Failure::DirectorySync;
        {
            InjectFailure inject(point);
            BOOST_CHECK_EXCEPTION(
                fileio::atomic_write(scratch.root / "state", [](std::ostream& output) {
                    output << "new";
                }),
                fileio::AtomicWriteError,
                [published](const fileio::AtomicWriteError& error) {
                    return error.published() == published
                        && error.code() == std::error_code(EIO, std::generic_category());
                });
        }
        BOOST_TEST(scratch.read() == (published ? "new" : "old"));
        scratch.no_temporary_files();
    }
}

BOOST_AUTO_TEST_CASE(snapshot_errors_preserve_publication_state) {
    Scratch scratch;
    model_io::AgentInputState state;
    for (const auto format : {load::StateFormat::Json, load::StateFormat::Readable}) {
        for (const auto point : {Failure::FileSync, Failure::DirectorySync}) {
            fileio::atomic_write(scratch.root / "state", [](std::ostream& output) {
                output << "old";
            });
            const bool published = point == Failure::DirectorySync;
            {
                InjectFailure inject(point);
                BOOST_CHECK_EXCEPTION(
                    load::save_state(scratch.root / "state", state, format),
                    load::PersistenceError,
                    [published](const load::PersistenceError& error) {
                        return error.published() == published;
                    });
            }
            if (published) {
                BOOST_TEST(scratch.read() != "old");
                if (format == load::StateFormat::Json) {
                    const nlohmann::json restored = load::load_state(scratch.root / "state");
                    BOOST_CHECK(restored == nlohmann::json(state));
                }
            } else {
                BOOST_TEST(scratch.read() == "old");
            }
            scratch.no_temporary_files();
        }
    }
    BOOST_CHECK_EXCEPTION(
        static_cast<void>(load::load_state(scratch.root / "missing")),
        load::PersistenceError,
        [](const load::PersistenceError& error) { return !error.published(); });
}
