#define BOOST_TEST_MODULE TextStreamEdit
#include <boost/test/unit_test.hpp>

#include "textedit/edit.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;

/// Maximum bytes one interposed read() returns; zero restores normal calls.
std::size_t read_cap = 0;

/// Bytes moved per inode while counting, to prove the scan budget.
std::map<ino_t, std::size_t> counted_reads;
std::map<ino_t, std::size_t> counted_writes;
bool counting = false;

void count_transfer(std::map<ino_t, std::size_t>& table, int descriptor, ssize_t bytes)
{
    if (!counting || bytes <= 0) return;
    struct stat info {};
    if (::fstat(descriptor, &info) != 0) return;
    table[info.st_ino] += static_cast<std::size_t>(bytes);
}

/** Count every transfer for the duration of one library call. */
struct Counting {
    Counting()
    {
        counted_reads.clear();
        counted_writes.clear();
        counting = true;
    }
    ~Counting() { counting = false; }
    Counting(const Counting&) = delete;
    Counting& operator=(const Counting&) = delete;
};

/** Cap every read so terminators, scalars and matches cross chunk edges. */
struct TinyReads {
    explicit TinyReads(std::size_t bytes) : previous(read_cap) { read_cap = bytes; }
    ~TinyReads() { read_cap = previous; }
    std::size_t previous;
};

struct Scratch {
    fs::path root;

    Scratch()
    {
        auto pattern = (fs::temp_directory_path() / "simplex-stream-edit-XXXXXX").string();
        const auto directory = ::mkdtemp(pattern.data());
        if (!directory) throw std::runtime_error("cannot create stream edit directory");
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

    void no_temporary_files() const
    {
        for (const auto& entry : fs::directory_iterator(root)) {
            BOOST_TEST(!entry.path().filename().string().starts_with(".simplex-edit-"));
        }
    }
};

/** Result fields that must agree between the in-memory and streaming paths. */
struct Observed {
    textedit::EditStatus status = textedit::EditStatus::NotFound;
    std::size_t matches_at_least = 0;
    std::size_t match_byte = 0;
    std::size_t second_match_byte = 0;
    std::size_t before_line = 0;
    std::size_t before_column = 0;
    std::size_t after_line = 0;
    std::size_t after_column = 0;
    std::size_t before_bytes = 0;
    std::size_t after_bytes = 0;
    bool preview_truncated = false;
    std::string before_excerpt;
    std::string after_excerpt;
    std::string file;

    bool operator==(const Observed&) const = default;
};

std::string hex(std::string_view bytes)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string text;
    for (unsigned char byte : bytes) {
        text += digits[byte >> 4];
        text += digits[byte & 0x0f];
        text += ' ';
    }
    return text;
}

Observed from_prepared(
    const textedit::PreparedEdit& plan, const std::string& file)
{
    const auto& result = plan.result;
    return {result.status, result.matches_at_least, result.match_byte,
            result.second_match_byte.value_or(0), result.before_position.line,
            result.before_position.column, result.after_position.line,
            result.after_position.column, result.before_bytes, result.after_bytes,
            result.preview_truncated, result.before_excerpt, result.after_excerpt, file};
}

Observed from_file(
    const textedit::EditResult& result, std::string file)
{
    return {result.status, result.matches_at_least, result.match_byte,
            result.second_match_byte.value_or(0), result.before_position.line,
            result.before_position.column, result.after_position.line,
            result.after_position.column, result.before_bytes, result.after_bytes,
            result.preview_truncated, result.before_excerpt, result.after_excerpt,
            std::move(file)};
}

void require_same(const Observed& streamed, const Observed& reference,
                  const std::string& context)
{
    BOOST_TEST_INFO("case " << context);
    if (streamed == reference) return;
    BOOST_CHECK_EQUAL(static_cast<int>(streamed.status), static_cast<int>(reference.status));
    BOOST_CHECK_EQUAL(streamed.matches_at_least, reference.matches_at_least);
    BOOST_CHECK_EQUAL(streamed.match_byte, reference.match_byte);
    BOOST_CHECK_EQUAL(streamed.second_match_byte, reference.second_match_byte);
    BOOST_CHECK_EQUAL(streamed.before_line, reference.before_line);
    BOOST_CHECK_EQUAL(streamed.before_column, reference.before_column);
    BOOST_CHECK_EQUAL(streamed.after_line, reference.after_line);
    BOOST_CHECK_EQUAL(streamed.after_column, reference.after_column);
    BOOST_CHECK_EQUAL(streamed.before_bytes, reference.before_bytes);
    BOOST_CHECK_EQUAL(streamed.after_bytes, reference.after_bytes);
    BOOST_CHECK_EQUAL(streamed.preview_truncated, reference.preview_truncated);
    BOOST_TEST_INFO("before '" << hex(streamed.before_excerpt) << "' vs '"
                   << hex(reference.before_excerpt) << "'");
    BOOST_CHECK_EQUAL(streamed.before_excerpt, reference.before_excerpt);
    BOOST_TEST_INFO("after '" << hex(streamed.after_excerpt) << "' vs '"
                   << hex(reference.after_excerpt) << "'");
    BOOST_CHECK_EQUAL(streamed.after_excerpt, reference.after_excerpt);
    BOOST_TEST_INFO("file '" << hex(streamed.file) << "' vs '"
                   << hex(reference.file) << "'");
    BOOST_CHECK_EQUAL(streamed.file, reference.file);
}

struct Case {
    std::string text;
    std::string old_text;
    std::string new_text;
};

std::vector<Case> cases()
{
    std::string long_line(900, 'x');
    long_line.insert(500, "needle");
    std::string many_lines;
    for (int line = 0; line < 60; ++line) many_lines += "line\n";
    std::string deep_lines;
    for (int line = 0; line < 30; ++line) deep_lines += "lead\n";
    deep_lines += "needle\n";
    for (int line = 0; line < 12; ++line) deep_lines += "tail\n";
    std::string long_span = "head\n";
    for (int line = 0; line < 100; ++line) long_span += "block\n";
    long_span += "tail\n";
    return {
        {"", "a", "b"},
        {"a", "a", "b"},
        {"a", "a", "a"},
        {"a", "\n", "b"},
        {"old", "old", ""},
        {"a\nold\nb\n", "old", "new"},
        {"before\r\nold\n", "old", "new"},
        {"old\r\nold", "old", "new"},
        {"a\r\nold\r\nb\r\n", "old\r\n", "new\r\n"},
        {"x\xe2\x80\xa8" "old\xe2\x80\xa9y", "old", "new"},
        {"a\xc2\x85old\xc2\x85", "old", "new"},
        {std::string("\x80\x80old\x80", 9), "old", "\xef\xbf\xbd"},
        {long_line, "needle", "REPLACED"},
        {long_line, long_line.substr(100, 300), "short"},
        {long_span, long_span.substr(5, 400), "new\n"},
        {long_span, "block\nblock\n", ""},
        {many_lines, "line\nline\n", "merged\n"},
        {many_lines, "line", "LINE"},
        {many_lines, "absent", "x"},
        // Matches far from the start, so captured row numbers must be absolute.
        {deep_lines, "needle", "changed"},
        {deep_lines, "needle", ""},
        {deep_lines, "needle\n", "one\ntwo\n"},
        {deep_lines, "lead\nlead\nlead", "merged\n"},
        {deep_lines, "tail\ntail", "TAIL\n"},
        {"aa", "a", "b"},
        {"aaa", "aa", "b"},
        {"abcabc", "abc", "abc"},
        {"abcabc", "abc", "xbc"},
        {"a\nb\nc\n", "a\nb\nc", "c\nb\na"},
        {std::string("x\n") + "old", "old", "new"},
        {std::string(4096, 'q'), "q", "qq"},
        {"trailing\r", "trailing\r", "done\r\n"},
        {"only\r", "only", "just"},
    };
}
}

/** Executable symbol interposition affects only this test's shared libraries. */
extern "C" ssize_t read(int descriptor, void* buffer, size_t count)
{
    if (read_cap != 0 && count > read_cap) count = read_cap;
    const auto result = static_cast<ssize_t>(::syscall(SYS_read, descriptor, buffer, count));
    count_transfer(counted_reads, descriptor, result);
    return result;
}

extern "C" ssize_t write(int descriptor, const void* buffer, size_t count)
{
    const auto result = static_cast<ssize_t>(::syscall(SYS_write, descriptor, buffer, count));
    count_transfer(counted_writes, descriptor, result);
    return result;
}

BOOST_FIXTURE_TEST_CASE(file_edits_match_in_memory_planning_at_every_chunk_edge, Scratch)
{
    const std::vector<std::size_t> caps{1, 2, 3, 5, 65536};
    const std::vector<std::size_t> contexts{0, 1, 3, 20};
    for (const auto& test : cases()) {
        for (const auto context : contexts) {
            const auto plan = textedit::prepare_str_replace(
                test.text, test.old_text, test.new_text, context);
            const auto expected_file = plan.result.status == textedit::EditStatus::Modified
                ? plan.updated_text
                : test.text;
            for (const auto cap : caps) {
                TinyReads tiny(cap);
                const auto path = write(test.text);
                const auto result = textedit::str_replace_file(
                    path, test.old_text, test.new_text, context);
                BOOST_REQUIRE(fs::exists(path));
                const auto context_text = "cap=" + std::to_string(cap)
                    + " size=" + std::to_string(test.text.size())
                    + " old=" + hex(test.old_text)
                    + " new=" + hex(test.new_text)
                    + " context=" + std::to_string(context);
                require_same(from_file(result, read(path)),
                             from_prepared(plan, expected_file), context_text);
                no_temporary_files();
            }
            BOOST_TEST(read_cap == 0u);
        }
    }
}

BOOST_FIXTURE_TEST_CASE(large_file_edit_stays_bounded_and_exact, Scratch)
{
    // Just above the former 16 MiB ceiling, generated in blocks instead of a
    // single fixture string, so the test process stays small.
    const auto path = root / "large";
    constexpr std::size_t block_bytes = 64 * 1024;
    constexpr std::size_t blocks = 288;
    {
        std::ofstream stream(path, std::ios::binary);
        std::string block(block_bytes, 'a');
        block.back() = '\n';
        for (std::size_t index = 0; index < blocks; ++index) {
            stream.write(block.data(), static_cast<std::streamsize>(block.size()));
        }
        stream.write("needle\n", 7);
        stream.close();
        BOOST_REQUIRE(stream.good());
    }
    const auto limit = std::size_t{20} * 1024 * 1024;
    const auto edited = textedit::str_replace_file(path, "needle", "REPLACED", 2, limit);
    BOOST_CHECK(edited.status == textedit::EditStatus::Modified);
    BOOST_TEST(edited.before_bytes == block_bytes * blocks + 7);
    BOOST_TEST(edited.after_bytes == block_bytes * blocks + 9);
    BOOST_TEST(edited.before_position.line == blocks);
    BOOST_TEST(read(path).size() == edited.after_bytes);
    BOOST_CHECK_THROW((void)textedit::str_replace_file(path, "REPLACED", "x", 2,
                       edited.after_bytes - 1), std::length_error);
    no_temporary_files();
}

BOOST_FIXTURE_TEST_CASE(unsafe_targets_and_limits_are_rejected_without_writing, Scratch)
{
    const auto path = write("old");
    fs::create_symlink(path, root / "link");
    BOOST_CHECK_THROW((void)textedit::str_replace_file(root / "link", "old", "new"),
                      std::system_error);
    fs::create_hard_link(path, root / "hardlink");
    BOOST_CHECK_THROW((void)textedit::str_replace_file(path, "old", "new"),
                      std::system_error);
    fs::remove(root / "hardlink");
    BOOST_CHECK_THROW((void)textedit::str_replace_file(path, "", "new"),
                      std::invalid_argument);
    BOOST_CHECK_THROW((void)textedit::str_replace_file(path, "old", "new", 21),
                      std::invalid_argument);
    BOOST_CHECK_THROW((void)textedit::str_replace_file(
        path, "old", "new", 3,
        std::numeric_limits<std::size_t>::max()), std::invalid_argument);
    BOOST_TEST(read(path) == "old");
    no_temporary_files();
}

BOOST_FIXTURE_TEST_CASE(successful_edit_keeps_the_documented_scan_budget, Scratch)
{
    const std::string text = std::string(300000, 'a') + "needle" + std::string(150000, 'b');
    const auto path = write(text);
    struct stat source {};
    BOOST_REQUIRE(::stat(path.c_str(), &source) == 0);
    textedit::EditResult result;
    {
        Counting counted;
        result = textedit::str_replace_file(path, "needle", "REPLACED", 3);
    }
    BOOST_REQUIRE(result.status == textedit::EditStatus::Modified);
    const auto edited_bytes = text.size() - 6 + 8;
    // Two source reads (scan + verification), one candidate read and one
    // candidate write; no hidden snapshot or preview pass.
    BOOST_TEST(counted_reads.size() == 2u);
    BOOST_TEST(counted_writes.size() == 1u);
    BOOST_TEST(counted_reads[source.st_ino] == 2 * text.size());
    const auto candidate = counted_writes.begin()->first;
    BOOST_TEST(counted_writes[candidate] == edited_bytes);
    BOOST_TEST(counted_reads[candidate] == edited_bytes);
    BOOST_TEST(read(path).size() == edited_bytes);
}

BOOST_FIXTURE_TEST_CASE(failed_matches_stop_reading_and_never_write_the_target, Scratch)
{
    const auto path = write("one\ntwo\n");
    struct stat source {};
    BOOST_REQUIRE(::stat(path.c_str(), &source) == 0);
    textedit::EditResult result;
    {
        Counting counted;
        result = textedit::str_replace_file(path, "absent", "x", 3);
        BOOST_CHECK(result.status == textedit::EditStatus::NotFound);
        // A missing match still scans the source once, but writes nothing.
        BOOST_TEST(counted_writes.empty());
        BOOST_TEST(counted_reads[source.st_ino] == 8u);
    }
    {
        Counting counted;
        const auto ambiguous = textedit::str_replace_file(path, "o", "0", 3);
        BOOST_CHECK(ambiguous.status == textedit::EditStatus::Ambiguous);
        BOOST_TEST(counted_writes.empty());
    }
    BOOST_TEST(read(path) == "one\ntwo\n");
    no_temporary_files();
}
