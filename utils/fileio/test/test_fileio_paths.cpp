#define BOOST_TEST_MODULE FileioPaths
#include <boost/test/unit_test.hpp>

#include "fileio/child_path.hpp"
#include "fileio/directory_tree_lock.hpp"
#include "fileio/session_lock.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

/** Each test owns a private tree and restores permissions before cleanup. */
struct Scratch {
    fs::path root;
    Scratch() {
        std::string pattern = (fs::temp_directory_path() / "simplex-fileio-XXXXXX").string();
        const auto* created = ::mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("mkdtemp failed");
        root = created;
    }
    ~Scratch() {
        std::error_code ignored;
        fs::permissions(root, fs::perms::owner_all, fs::perm_options::add, ignored);
        fs::remove_all(root, ignored);
    }
};

}

BOOST_FIXTURE_TEST_CASE(child_path_stays_below_its_root, Scratch) {
    BOOST_TEST(fileio::child_path(root, "state") == root / "state");
    BOOST_TEST(fileio::child_path(root, "nested/state") == root / "nested" / "state");
    BOOST_CHECK_THROW(fileio::child_path(root, ""), std::invalid_argument);
    BOOST_CHECK_THROW(fileio::child_path(root, "../escape"), std::invalid_argument);
    BOOST_CHECK_THROW(fileio::child_path(root, "/absolute"), std::invalid_argument);
    const std::string embedded_nul("a\0b", 3);
    BOOST_CHECK_THROW(fileio::child_path(root, embedded_nul), std::invalid_argument);
}

BOOST_FIXTURE_TEST_CASE(directory_tree_lock_accepts_a_normal_tree, Scratch) {
    const auto directory = root / "a" / "b";
    fs::create_directories(directory);
    BOOST_CHECK_NO_THROW(fileio::DirectoryTreeLock lock(directory));
}

BOOST_FIXTURE_TEST_CASE(directory_tree_lock_names_a_symlinked_ancestor, Scratch) {
    const auto real = root / "real";
    fs::create_directories(real / "child");
    const auto link = root / "link";
    fs::create_directory_symlink(real, link);
    const auto target = link / "child";
    BOOST_CHECK_EXCEPTION(fileio::DirectoryTreeLock lock(target), std::invalid_argument,
        [](const auto& error) {
            return std::string(error.what()).find("ordinary directories") != std::string::npos;
        });
}

BOOST_FIXTURE_TEST_CASE(shared_session_lock_opens_a_read_only_file, Scratch) {
    const auto lock = root / "session.lock";
    { std::ofstream(lock) << ""; }
    BOOST_REQUIRE(::chmod(lock.c_str(), 0444) == 0);
    BOOST_CHECK_NO_THROW(fileio::SessionLock reader(lock, fileio::SessionLock::Mode::Shared));
}

BOOST_FIXTURE_TEST_CASE(shared_session_lock_blocks_exclusive_ownership, Scratch) {
    const auto lock = root / "session.lock";
    fileio::SessionLock reader(lock, fileio::SessionLock::Mode::Shared);
    BOOST_CHECK_THROW(fileio::SessionLock writer(lock), std::system_error);
}
