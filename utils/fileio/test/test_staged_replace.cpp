#define BOOST_TEST_MODULE FileioStagedReplace
#include <boost/test/unit_test.hpp>

#include "fileio/staged_replace.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;

struct Scratch {
    fs::path root;

    Scratch()
    {
        auto pattern = (fs::temp_directory_path() / "simplex-staged-XXXXXX").string();
        const auto directory = ::mkdtemp(pattern.data());
        if (!directory) throw std::runtime_error("cannot create staged replace directory");
        root = directory;
    }

    ~Scratch()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    fs::path write(const std::string& bytes) const
    {
        const auto path = root / "source";
        std::ofstream stream(path, std::ios::binary);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        BOOST_REQUIRE(stream.good());
        return path;
    }

    std::string read(const fs::path& path) const
    {
        std::ifstream stream(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(stream), {}};
    }

    std::size_t candidates() const
    {
        std::size_t count = 0;
        for (const auto& entry : fs::directory_iterator(root)) {
            if (entry.path().filename().string().starts_with(".simplex-edit-")) ++count;
        }
        return count;
    }
};

/** Records the offsets and bytes the verification pass reports. */
struct Recorder final : fileio::SourceObserver {
    struct Chunk {
        std::uintmax_t offset;
        std::string bytes;
    };
    std::vector<Chunk> chunks;

    void observe(std::uintmax_t offset, std::string_view bytes) override
    {
        chunks.push_back({offset, std::string(bytes)});
    }

    std::string joined() const
    {
        std::string text;
        for (const auto& chunk : chunks) text += chunk.bytes;
        return text;
    }
};

std::string scan(fileio::StagedReplacement& staged)
{
    std::string text;
    for (;;) {
        const auto chunk = staged.next_source();
        if (chunk.empty()) break;
        text.append(chunk);
    }
    return text;
}
}

BOOST_FIXTURE_TEST_CASE(candidate_exists_only_while_it_holds_bytes, Scratch)
{
    const auto path = write("headMATCHtail");
    fileio::StagedReplacement staged(path);
    // Construction applies the editable-file policy but touches nothing else.
    BOOST_TEST(candidates() == 0u);
    BOOST_TEST(scan(staged) == "headMATCHtail");
    BOOST_TEST(staged.source_bytes() == 13u);
    BOOST_TEST(candidates() == 0u);
    staged.copy_prefix(4);
    BOOST_TEST(candidates() == 1u);
    staged.discard();
    BOOST_TEST(candidates() == 0u);
    BOOST_TEST(read(path) == "headMATCHtail");
}

BOOST_FIXTURE_TEST_CASE(copied_prefix_and_suffix_publish_the_expected_file, Scratch)
{
    const auto path = write("headMATCHtail");
    fileio::StagedReplacement staged(path);
    BOOST_TEST(scan(staged) == "headMATCHtail");
    staged.copy_prefix(4);
    staged.write("NEW");
    staged.write("tail");
    BOOST_TEST(staged.candidate_bytes() == 11u);
    Recorder recorder;
    staged.publish(4, "MATCH", &recorder);
    BOOST_TEST(read(path) == "headNEWtail");
    BOOST_TEST(candidates() == 0u);
    // The verifier reports the untouched prefix and suffix, never the span.
    BOOST_TEST(recorder.joined() == "headtail");
    BOOST_REQUIRE(recorder.chunks.size() == 2u);
    BOOST_TEST(recorder.chunks[0].offset == 0u);
    BOOST_TEST(recorder.chunks[1].offset == 9u);
}

BOOST_FIXTURE_TEST_CASE(empty_candidate_replaces_the_file_with_nothing, Scratch)
{
    const auto path = write("gone");
    fileio::StagedReplacement staged(path);
    BOOST_TEST(scan(staged) == "gone");
    BOOST_TEST(candidates() == 0u);
    staged.publish(0, "gone");
    BOOST_TEST(read(path).empty());
    BOOST_TEST(candidates() == 0u);
}

BOOST_FIXTURE_TEST_CASE(restart_source_reads_the_same_bytes_again, Scratch)
{
    const auto path = write("first\nsecond\n");
    fileio::StagedReplacement staged(path);
    BOOST_TEST(scan(staged) == "first\nsecond\n");
    staged.restart_source();
    BOOST_TEST(staged.source_bytes() == 0u);
    BOOST_TEST(scan(staged) == "first\nsecond\n");
    BOOST_TEST(staged.source_bytes() == 13u);
    BOOST_TEST(candidates() == 0u);
}

BOOST_FIXTURE_TEST_CASE(publish_rejects_a_second_use, Scratch)
{
    const auto path = write("old");
    fileio::StagedReplacement staged(path);
    BOOST_TEST(scan(staged) == "old");
    staged.copy_prefix(0);
    staged.write("new");
    staged.publish(0, "old");
    BOOST_CHECK_THROW(staged.publish(0, "old"), std::logic_error);
}
