#define BOOST_TEST_MODULE AtomicWrite
#include <boost/test/unit_test.hpp>

#include "fileio/atomic_write.hpp"

#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

/** Per-test disk tree; no fixture depends on JSON or any application package. */
struct Scratch {
    fs::path root = fs::temp_directory_path()
        / ("simplex_atomic_write_" + std::to_string(::getpid()));

    Scratch() {
        fs::remove_all(root);
        fs::create_directories(root);
    }

    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    std::string read(const fs::path& relative) const {
        std::ifstream input(root / relative, std::ios::binary);
        BOOST_REQUIRE(input.good());
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    void no_temporary_files() const {
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            BOOST_TEST(!entry.path().filename().string().starts_with(".simplex-write-"));
        }
    }
};

/** Distinct exception type verifies callback failures cross the utility intact. */
struct WriterFailure : std::runtime_error {
    WriterFailure() : std::runtime_error("producer failed") {}
};

} // namespace

BOOST_AUTO_TEST_CASE(streams_binary_data_across_buffers_and_replaces_with_empty_file) {
    Scratch scratch;
    std::string bytes(40000, '\0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>(index % 256);
    }
    int calls = 0;
    fileio::atomic_write(scratch.root / "nested/output", [&](std::ostream& output) {
        ++calls;
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    });
    BOOST_TEST(calls == 1);
    BOOST_TEST(scratch.read("nested/output") == bytes);
    const auto permissions = fs::status(scratch.root / "nested/output").permissions();
    BOOST_CHECK((permissions & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);

    fileio::atomic_write(scratch.root / "nested/output", [](std::ostream&) {});
    BOOST_TEST(scratch.read("nested/output").empty());
    scratch.no_temporary_files();
}

BOOST_AUTO_TEST_CASE(callback_and_stream_failures_preserve_previous_contents) {
    Scratch scratch;
    const auto file = scratch.root / "output";
    fileio::atomic_write(file, [](std::ostream& output) { output << "old"; });

    BOOST_CHECK_THROW(fileio::atomic_write(file, [](std::ostream& output) {
        output << std::string(20000, 'x');
        throw WriterFailure();
    }), WriterFailure);
    BOOST_TEST(scratch.read("output") == "old");

    BOOST_CHECK_THROW(fileio::atomic_write(file, [](std::ostream& output) {
        output << "partial";
        output.exceptions(std::ios::goodbit);
        output.setstate(std::ios::badbit);
    }), std::ios_base::failure);
    BOOST_TEST(scratch.read("output") == "old");

    BOOST_CHECK_THROW(fileio::atomic_write(scratch.root / "absent", [](std::ostream&) {
        throw WriterFailure();
    }), WriterFailure);
    BOOST_TEST(!fs::exists(scratch.root / "absent"));
    scratch.no_temporary_files();
}

BOOST_AUTO_TEST_CASE(rename_failure_cleans_up_without_modifying_the_destination) {
    Scratch scratch;
    fs::create_directory(scratch.root / "occupied");
    fileio::atomic_write(scratch.root / "occupied/keep", [](std::ostream& output) {
        output << "preserved";
    });
    BOOST_CHECK_THROW(fileio::atomic_write(scratch.root / "occupied", [](std::ostream& output) {
        output << "replacement";
    }), fs::filesystem_error);
    BOOST_TEST(scratch.read("occupied/keep") == "preserved");
    scratch.no_temporary_files();
}

BOOST_AUTO_TEST_CASE(replaces_a_symlink_without_touching_its_target) {
    Scratch scratch;
    fileio::atomic_write(scratch.root / "target", [](std::ostream& output) {
        output << "original target";
    });
    fs::create_symlink("target", scratch.root / "link");
    fileio::atomic_write(scratch.root / "link", [](std::ostream& output) {
        output << "new destination";
    });
    BOOST_TEST(!fs::is_symlink(scratch.root / "link"));
    BOOST_TEST(scratch.read("target") == "original target");
    BOOST_TEST(scratch.read("link") == "new destination");
    scratch.no_temporary_files();
}

BOOST_AUTO_TEST_CASE(empty_callback_is_rejected_before_creating_directories) {
    Scratch scratch;
    BOOST_CHECK_THROW(fileio::atomic_write(scratch.root / "missing/output", {}), std::invalid_argument);
    BOOST_TEST(!fs::exists(scratch.root / "missing"));
}


BOOST_AUTO_TEST_CASE(temporary_descriptor_is_close_on_exec_during_callback) {
    Scratch scratch;
    fileio::atomic_write(scratch.root / "output", [&](std::ostream& output) {
        bool found = false;
        for (const auto& entry : fs::directory_iterator("/proc/self/fd")) {
            std::error_code ignored;
            const auto target = fs::read_symlink(entry.path(), ignored);
            if (ignored || target.parent_path() != scratch.root
                || !target.filename().string().starts_with(".simplex-write-")) {
                continue;
            }
            found = true;
            const int descriptor = std::stoi(entry.path().filename().string());
            const int flags = ::fcntl(descriptor, F_GETFD);
            BOOST_REQUIRE(flags >= 0);
            BOOST_TEST((flags & FD_CLOEXEC) != 0);
        }
        BOOST_REQUIRE(found);
        output << "complete";
    });
}
