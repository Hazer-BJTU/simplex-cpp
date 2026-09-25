#define BOOST_TEST_MODULE TextEditInitialConflict
#include <boost/test/unit_test.hpp>

#include "textedit/edit.hpp"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
bool change_during_read = false;
ino_t target_inode = 0;

struct Scratch {
    std::filesystem::path root;

    Scratch()
    {
        auto pattern = (std::filesystem::temp_directory_path() /
                        "simplex-edit-read-race-XXXXXX").string();
        const auto directory = ::mkdtemp(pattern.data());
        if (!directory) throw std::runtime_error("cannot create test directory");
        root = directory;
    }

    ~Scratch()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};
}

/** Change metadata after the first source read, before its post-read fstat. */
extern "C" ssize_t read(int descriptor, void* buffer, size_t count)
{
    const auto result = static_cast<ssize_t>(::syscall(SYS_read, descriptor, buffer, count));
    if (change_during_read && result > 0) {
        struct stat info {};
        if (::fstat(descriptor, &info) == 0 && info.st_ino == target_inode) {
            change_during_read = false;
            if (::fchmod(descriptor, 0640) != 0) {
                errno = EIO;
                return -1;
            }
        }
    }
    return result;
}

BOOST_AUTO_TEST_CASE(initial_snapshot_conflict_returns_status_without_writing)
{
    Scratch scratch;
    const auto path = scratch.root / "source";
    {
        std::ofstream output(path, std::ios::binary);
        output << "old";
        BOOST_REQUIRE(output.good());
    }
    BOOST_REQUIRE(::chmod(path.c_str(), 0600) == 0);
    struct stat original {};
    BOOST_REQUIRE(::stat(path.c_str(), &original) == 0);
    target_inode = original.st_ino;
    change_during_read = true;

    const auto result = textedit::str_replace_file(path, "old", "new");
    BOOST_CHECK(!change_during_read);
    BOOST_CHECK(result.status == textedit::EditStatus::Conflict);
    BOOST_TEST(result.before_excerpt.empty());
    BOOST_TEST(result.after_excerpt.empty());
    std::ifstream input(path, std::ios::binary);
    BOOST_TEST(std::string(std::istreambuf_iterator<char>(input), {}) == "old");
    for (const auto& entry : std::filesystem::directory_iterator(scratch.root)) {
        BOOST_TEST(!entry.path().filename().string().starts_with(".simplex-edit-"));
    }
}
