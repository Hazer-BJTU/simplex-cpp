#define BOOST_TEST_MODULE TextEditPublication
#include <boost/test/unit_test.hpp>

#include "textedit/edit.hpp"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
enum class Failure { None, FileSync, DirectorySync };
Failure failure = Failure::None;
std::stop_source* cancel_source = nullptr;
bool cancel_on_directory = false;

struct InjectFailure {
    explicit InjectFailure(Failure value) { failure = value; }
    ~InjectFailure() { failure = Failure::None; }
};

struct Scratch {
    std::filesystem::path root;

    Scratch()
    {
        auto pattern = (std::filesystem::temp_directory_path() / "simplex-edit-sync-XXXXXX").string();
        const auto directory = ::mkdtemp(pattern.data());
        if (!directory) throw std::runtime_error("cannot create test directory");
        root = directory;
    }

    ~Scratch()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    void write(const std::string& text) const
    {
        std::ofstream output(root / "source", std::ios::binary);
        output << text;
        BOOST_REQUIRE(output.good());
    }

    std::string read() const
    {
        std::ifstream input(root / "source", std::ios::binary);
        return {std::istreambuf_iterator<char>(input), {}};
    }

    void no_temporary_files() const
    {
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            BOOST_TEST(!entry.path().filename().string().starts_with(".simplex-edit-"));
        }
    }
};
}

/** Executable symbol interposition affects only this test's shared libraries. */
extern "C" int fsync(int descriptor)
{
    struct stat status {};
    if (::fstat(descriptor, &status) == 0) {
        const bool directory = S_ISDIR(status.st_mode);
        if (cancel_source && directory == cancel_on_directory) cancel_source->request_stop();
        if ((directory && failure == Failure::DirectorySync) ||
            (!directory && failure == Failure::FileSync)) {
            errno = EIO;
            return -1;
        }
    }
    return static_cast<int>(::syscall(SYS_fsync, descriptor));
}

BOOST_AUTO_TEST_CASE(file_sync_failure_leaves_original_and_cleans_temporary)
{
    Scratch scratch;
    scratch.write("old");
    {
        InjectFailure inject(Failure::FileSync);
        BOOST_CHECK_THROW((void)textedit::str_replace_file(
            scratch.root / "source", "old", "new"), std::system_error);
    }
    BOOST_TEST(scratch.read() == "old");
    scratch.no_temporary_files();
}

BOOST_AUTO_TEST_CASE(directory_sync_failure_reports_visible_publication)
{
    Scratch scratch;
    scratch.write("old");
    {
        InjectFailure inject(Failure::DirectorySync);
        const auto result = textedit::str_replace_file(
            scratch.root / "source", "old", "new");
        BOOST_CHECK(result.status == textedit::EditStatus::PublishedSyncFailed);
        BOOST_TEST(result.persistence_error == std::error_code(EIO, std::generic_category()));
        BOOST_TEST(result.after_excerpt == "+ 0 | new");
    }
    BOOST_TEST(scratch.read() == "new");
    scratch.no_temporary_files();
}

BOOST_AUTO_TEST_CASE(cancellation_at_file_sync_prevents_publication)
{
    Scratch scratch;
    scratch.write("old");
    std::stop_source stop;
    struct Reset { ~Reset() { cancel_source = nullptr; } } reset;
    cancel_source = &stop;
    cancel_on_directory = false;
    BOOST_CHECK_EXCEPTION((void)textedit::str_replace_file(
        scratch.root / "source", "old", "new", 3, 1024, stop.get_token()),
        std::system_error, [](const auto& error) {
            return error.code() == std::errc::operation_canceled;
        });
    BOOST_TEST(scratch.read() == "old");
    scratch.no_temporary_files();
}

BOOST_AUTO_TEST_CASE(cancellation_after_publication_does_not_hide_result_or_sync_failure)
{
    for (const auto failure_mode : {Failure::None, Failure::DirectorySync}) {
        Scratch scratch;
        scratch.write("old");
        std::stop_source stop;
        struct Reset { ~Reset() { cancel_source = nullptr; } } reset;
        cancel_source = &stop;
        cancel_on_directory = true;
        InjectFailure inject(failure_mode);
        const auto result = textedit::str_replace_file(
            scratch.root / "source", "old", "new", 3, 1024, stop.get_token());
        BOOST_TEST(stop.stop_requested());
        BOOST_CHECK(result.status == (failure_mode == Failure::None
            ? textedit::EditStatus::Modified : textedit::EditStatus::PublishedSyncFailed));
        BOOST_TEST(scratch.read() == "new");
        scratch.no_temporary_files();
    }
}
