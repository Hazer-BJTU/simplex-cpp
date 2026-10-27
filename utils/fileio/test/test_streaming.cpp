#define BOOST_TEST_MODULE FileioStreaming
#include <boost/test/unit_test.hpp>

#include "fileio/streaming.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;

struct Scratch {
    fs::path root;

    Scratch()
    {
        auto pattern = (fs::temp_directory_path() / "simplex-stream-XXXXXX").string();
        const auto directory = ::mkdtemp(pattern.data());
        if (!directory) throw std::runtime_error("cannot create streaming test directory");
        root = directory;
    }

    ~Scratch()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    fs::path write(const std::string& bytes) const
    {
        const auto path = root / "input";
        std::ofstream stream(path, std::ios::binary);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        BOOST_REQUIRE(stream.good());
        return path;
    }
};

std::string drain(fileio::SequentialFile& file, std::size_t* chunks = nullptr)
{
    std::string bytes;
    std::size_t seen = 0;
    for (auto chunk = file.next(); !chunk.empty(); chunk = file.next()) {
        bytes.append(chunk.data(), chunk.size());
        ++seen;
    }
    if (chunks != nullptr) *chunks = seen;
    return bytes;
}
}

BOOST_FIXTURE_TEST_CASE(sequential_file_streams_whole_content_in_fixed_chunks, Scratch)
{
    const std::string content = "alpha\r\nbeta\xe4\xb8\xad" "gamma\n";
    const auto path = write(content);
    auto file = fileio::SequentialFile::open(path, 7);
    std::size_t chunks = 0;
    BOOST_TEST(drain(file, &chunks) == content);
    BOOST_TEST(chunks > 1u);
    BOOST_TEST(file.observed_bytes() == content.size());
    BOOST_REQUIRE(file.identity().size == content.size());
    BOOST_TEST(file.next().empty()); // EOF is sticky.
}

BOOST_FIXTURE_TEST_CASE(empty_files_and_limits_are_explicit, Scratch)
{
    const auto path = write("");
    auto file = fileio::SequentialFile::open(path, 8);
    BOOST_TEST(file.next().empty());
    BOOST_TEST(file.observed_bytes() == 0u);
    BOOST_CHECK_THROW((void)fileio::SequentialFile::open(path, 3), std::invalid_argument);
    BOOST_CHECK_THROW((void)fileio::SequentialFile::open(path, 2 * 1024 * 1024),
                      std::invalid_argument);
    const std::string nul_path("a\0b", 3);
    BOOST_CHECK_THROW((void)fileio::SequentialFile::open(fs::path(nul_path)),
                      std::invalid_argument);
}

BOOST_FIXTURE_TEST_CASE(non_regular_targets_and_missing_paths_throw, Scratch)
{
    const auto path = write("data");
    BOOST_CHECK_THROW((void)fileio::SequentialFile::open(root), std::system_error);
    BOOST_CHECK_THROW((void)fileio::SequentialFile::open(root / "missing"), std::system_error);
    BOOST_REQUIRE(::mkfifo((root / "fifo").c_str(), 0600) == 0);
    BOOST_CHECK_THROW((void)fileio::SequentialFile::open(root / "fifo"), std::system_error);

    fs::create_symlink(path, root / "link");
    auto file = fileio::SequentialFile::open(root / "link", 4);
    BOOST_TEST(drain(file) == "data");
}

BOOST_FIXTURE_TEST_CASE(moved_file_keeps_reading_and_releases_the_descriptor, Scratch)
{
    const auto path = write("0123456789");
    auto first = fileio::SequentialFile::open(path, 4);
    BOOST_TEST(first.next() == "0123");
    auto second = std::move(first);
    BOOST_TEST(drain(second) == "456789");
    BOOST_TEST(second.observed_bytes() == 10u);
}
