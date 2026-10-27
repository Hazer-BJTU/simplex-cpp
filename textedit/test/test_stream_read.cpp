#define BOOST_TEST_MODULE TextStreamRead
#include <boost/test/unit_test.hpp>

#include "textedit/read.hpp"

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
#include <vector>

namespace {
namespace fs = std::filesystem;
constexpr auto all = std::numeric_limits<std::size_t>::max();

/// Maximum bytes one interposed read() returns; zero restores normal calls.
std::size_t read_cap = 0;

/// Bytes read per inode while counting, to prove the single-scan budget.
std::map<ino_t, std::size_t> counted_reads;
bool counting = false;

/** Restore the normal read length even when an assertion throws. */
struct TinyReads {
    explicit TinyReads(std::size_t bytes) : previous(read_cap) { read_cap = bytes; }
    ~TinyReads() { read_cap = previous; }
    std::size_t previous;
};

struct Scratch {
    fs::path root;

    Scratch()
    {
        auto pattern = (fs::temp_directory_path() / "simplex-stream-read-XXXXXX").string();
        const auto directory = ::mkdtemp(pattern.data());
        if (!directory) throw std::runtime_error("cannot create stream read directory");
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

/** Public result fields, comparable across the string and file APIs. */
struct Snapshot {
    std::string text;
    std::size_t total_bytes = 0;
    std::size_t total_lines = 0;
    std::size_t start_byte = 0;
    std::size_t end_byte = 0;
    std::size_t start_line = 0;
    std::size_t lines_read = 0;
    bool reached_end = false;
    bool output_truncated = false;
    bool display_replaced = false;

    bool operator==(const Snapshot&) const = default;
};

Snapshot snapshot(const textedit::BoundedLineReadResult& result)
{
    return {result.text, result.total_bytes, result.total_lines, result.start_byte,
            result.end_byte, result.start_line, result.lines_read, result.reached_end,
            result.output_truncated, result.display_replaced};
}

Snapshot snapshot(const textedit::LineReadResult& result)
{
    return {result.text, result.total_bytes, result.total_lines, result.start_byte,
            result.end_byte, result.start_line, result.lines_read, result.reached_end,
            false, false};
}

Snapshot snapshot(const textedit::ReadResult& result)
{
    return {result.text, result.total_bytes, result.total_lines, result.start_byte,
            result.end_byte, 0, 0, result.reached_end, false, false};
}

enum class Outcome { Ok, OutOfRange, InvalidArgument, LengthError, Other };

struct Observed {
    Outcome outcome = Outcome::Other;
    Snapshot value;
};

template <typename Call>
Observed observe(Call&& call)
{
    try {
        return {Outcome::Ok, snapshot(call())};
    } catch (const std::length_error&) {
        return {Outcome::LengthError, {}};
    } catch (const std::out_of_range&) {
        return {Outcome::OutOfRange, {}};
    } catch (const std::invalid_argument&) {
        return {Outcome::InvalidArgument, {}};
    } catch (...) {
        return {Outcome::Other, {}};
    }
}

std::string describe(Outcome outcome)
{
    switch (outcome) {
        case Outcome::Ok: return "ok";
        case Outcome::OutOfRange: return "out_of_range";
        case Outcome::InvalidArgument: return "invalid_argument";
        case Outcome::LengthError: return "length_error";
        case Outcome::Other: return "other";
    }
    return "unknown";
}

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

void require_same(const Observed& streamed, const Observed& reference,
                  const std::string& context)
{
    BOOST_TEST_INFO("case " << context);
    BOOST_TEST_INFO("streamed=" << describe(streamed.outcome)
                   << " reference=" << describe(reference.outcome));
    BOOST_REQUIRE(streamed.outcome == reference.outcome);
    if (reference.outcome != Outcome::Ok) return;
    const auto& got = streamed.value;
    const auto& want = reference.value;
    if (got == want) return;
    BOOST_TEST_INFO("text '" << got.text << "' vs '" << want.text << "'");
    BOOST_TEST_INFO("hex " << hex(got.text) << "vs " << hex(want.text));
    BOOST_CHECK_EQUAL(got.total_bytes, want.total_bytes);
    BOOST_CHECK_EQUAL(got.total_lines, want.total_lines);
    BOOST_CHECK_EQUAL(got.start_byte, want.start_byte);
    BOOST_CHECK_EQUAL(got.end_byte, want.end_byte);
    BOOST_CHECK_EQUAL(got.start_line, want.start_line);
    BOOST_CHECK_EQUAL(got.lines_read, want.lines_read);
    BOOST_CHECK_EQUAL(got.reached_end, want.reached_end);
    BOOST_CHECK_EQUAL(got.output_truncated, want.output_truncated);
    BOOST_CHECK_EQUAL(got.display_replaced, want.display_replaced);
    BOOST_CHECK_EQUAL(got.text, want.text);
}

/** Terminators, invalid bytes, multibyte text and long lines in one corpus. */
std::vector<std::string> corpus()
{
    std::string long_line(1200, 'x');
    long_line.insert(600, "\xe4\xb8\xad");
    return {
        "",
        "a",
        "\n",
        "\n\n",
        "a\r\nb\rc\nD",
        "a\r",
        "\r\n\r\n",
        "a\xc2\x85" "b\xe2\x80\xa8" "c\xe2\x80\xa9" "d",
        "trailing\n",
        "trailing\r\n",
        "tab\there\x0b" "vt\x0c" "ff",
        "\xe4\xb8\xad\xe6\x96\x87\n\xf0\x9f\x99\x82",
        std::string("\x80\x80\x80\xc2\n\xe2", 6),
        "\xc2",
        "\xe2\x80",
        "\xe2\x80\xa8",
        "a\xe2\x80\xa8"
        "b",
        long_line,
        long_line + "\n" + long_line,
        "\xef\xbb\xbf" "bom\n",
        std::string(300, '\n'),
    };
}
}

/** Executable symbol interposition affects only this test's shared libraries. */
extern "C" ssize_t read(int descriptor, void* buffer, size_t count)
{
    if (read_cap != 0 && count > read_cap) count = read_cap;
    const auto result = static_cast<ssize_t>(::syscall(SYS_read, descriptor, buffer, count));
    if (counting && result > 0) {
        struct stat info {};
        if (::fstat(descriptor, &info) == 0) {
            counted_reads[info.st_ino] += static_cast<std::size_t>(result);
        }
    }
    return result;
}

BOOST_FIXTURE_TEST_CASE(line_reads_match_string_rendering_at_every_chunk_edge, Scratch)
{
    const std::vector<std::size_t> caps{1, 2, 5, 65536};
    const std::vector<std::size_t> budgets{0, 3, 17, 65536};
    for (const auto& text : corpus()) {
        const auto path = write(text);
        const auto lines = textedit::read_lines(text, 0, all).total_lines;
        for (const auto cap : caps) {
            TinyReads tiny(cap);
            for (const auto start : {std::size_t{0}, std::size_t{1}, std::size_t{2}, lines}) {
                for (const auto count : {std::size_t{0}, std::size_t{1}, std::size_t{3}, all}) {
                    for (const auto format : {textedit::LineReadFormat::Plain,
                                              textedit::LineReadFormat::LineIndex,
                                              textedit::LineReadFormat::ByteRange}) {
                        const auto context = "cap=" + std::to_string(cap)
                            + " size=" + std::to_string(text.size())
                            + " start=" + std::to_string(start)
                            + " count=" + std::to_string(count)
                            + " format=" + std::to_string(static_cast<int>(format));
                        const auto reference = observe([&] {
                            return textedit::read_lines(text, start, count, format);
                        });
                        const auto streamed = observe([&] {
                            return textedit::read_file_lines(path, start, count, format);
                        });
                        require_same(streamed, reference, context);
                        for (const auto budget : budgets) {
                            const auto ref = observe([&] {
                                return textedit::read_lines_bounded(
                                    text, start, count, format, budget);
                            });
                            const auto got = observe([&] {
                                return textedit::read_file_lines_bounded(
                                    path, start, count, format, budget);
                            });
                            require_same(got, ref, context + " budget=" + std::to_string(budget));
                        }
                    }
                }
            }
        }
        BOOST_TEST(read_cap == 0u);
    }
}

BOOST_FIXTURE_TEST_CASE(byte_reads_match_string_rendering_at_every_chunk_edge, Scratch)
{
    const std::vector<std::size_t> caps{1, 2, 5, 65536};
    const std::vector<std::size_t> budgets{0, 3, 17, 65536};
    for (const auto& text : corpus()) {
        const auto path = write(text);
        for (const auto cap : caps) {
            TinyReads tiny(cap);
            for (const auto start : {std::size_t{0}, std::size_t{1},
                                     text.size(), text.size() + 1}) {
                for (const auto count : {std::size_t{0}, std::size_t{1},
                                         std::size_t{3}, all}) {
                    for (const auto format : {textedit::ByteReadFormat::Plain,
                                              textedit::ByteReadFormat::HexEscaped}) {
                        const auto context = "cap=" + std::to_string(cap)
                            + " size=" + std::to_string(text.size())
                            + " start=" + std::to_string(start)
                            + " count=" + std::to_string(count)
                            + " hex=" + std::to_string(format == textedit::ByteReadFormat::HexEscaped);
                        const auto reference = observe([&] {
                            return textedit::read_bytes(text, start, count, format);
                        });
                        const auto streamed = observe([&] {
                            return textedit::read_file_bytes(path, start, count, format);
                        });
                        require_same(streamed, reference, context);
                        for (const auto budget : budgets) {
                            const auto ref = observe([&] {
                                return textedit::read_bytes_bounded(
                                    text, start, count, format, budget);
                            });
                            const auto got = observe([&] {
                                return textedit::read_file_bytes_bounded(
                                    path, start, count, format, budget);
                            });
                            require_same(got, ref, context + " budget=" + std::to_string(budget));
                        }
                    }
                }
            }
        }
        BOOST_TEST(read_cap == 0u);
    }
}

BOOST_FIXTURE_TEST_CASE(limits_and_selection_errors_survive_streaming, Scratch)
{
    const auto path = write("a\r\nb");
    BOOST_CHECK_THROW((void)textedit::read_file_lines(path, 0, 1,
                       textedit::LineReadFormat::Plain, 3), std::length_error);
    BOOST_CHECK_THROW((void)textedit::read_file_bytes(path, 0, 1,
                       textedit::ByteReadFormat::Plain, 3), std::length_error);
    BOOST_CHECK_THROW((void)textedit::read_file_lines(path, 0, 1,
                       textedit::LineReadFormat::Plain, all), std::invalid_argument);
    BOOST_CHECK_THROW((void)textedit::read_file_bytes(path, 5, 1), std::out_of_range);
    BOOST_CHECK_THROW((void)textedit::read_file_lines(path, 3, 1), std::out_of_range);
    const auto at_eof = textedit::read_file_lines(path, 2, 4);
    BOOST_TEST(at_eof.lines_read == 0u);
    BOOST_TEST(at_eof.start_byte == 4u);
    BOOST_TEST(at_eof.end_byte == 4u);
    BOOST_TEST(at_eof.reached_end);
    BOOST_TEST(textedit::read_file_bytes(path, 0, all).text == "a\r\nb");

    fs::create_symlink(path, root / "link");
    BOOST_TEST(textedit::read_file_lines(root / "link", 0, 1).text == "a\r\n");
    BOOST_CHECK_THROW((void)textedit::read_file_lines(root / "missing", 0, 1),
                      std::system_error);
    BOOST_CHECK_THROW((void)textedit::read_file_bytes(root, 0, 1), std::system_error);
    BOOST_REQUIRE(::mkfifo((root / "fifo").c_str(), 0600) == 0);
    BOOST_CHECK_THROW((void)textedit::read_file_lines(root / "fifo", 0, 1),
                      std::system_error);
}

BOOST_FIXTURE_TEST_CASE(large_dense_file_stays_bounded_with_final_counts, Scratch)
{
    // Just above the former 16 MiB ceiling: dense terminators keep totals exact
    // without building a large string in the test process.
    const auto path = root / "dense";
    {
        std::ofstream stream(path, std::ios::binary);
        std::string block(64 * 1024, '\n');
        for (int index = 0; index < 273; ++index) stream.write(block.data(),
                                                               static_cast<std::streamsize>(block.size()));
        stream.close();
        BOOST_REQUIRE(stream.good());
    }
    const auto lines = textedit::read_file_lines_bounded(
        path, 0, all, textedit::LineReadFormat::ByteRange, 4096, all - 1);
    BOOST_TEST(lines.total_bytes == 64u * 1024u * 273u);
    BOOST_TEST(lines.total_lines == lines.total_bytes + 1);
    BOOST_TEST(lines.output_truncated);
    BOOST_TEST(lines.text.size() <= 4096u);
    BOOST_TEST(lines.text.starts_with("[       0:       1] | "));
    const auto hex = textedit::read_file_bytes_bounded(
        path, 0, all, textedit::ByteReadFormat::HexEscaped, 4096, all - 1);
    BOOST_TEST(hex.total_lines == lines.total_lines);
    BOOST_TEST(hex.output_truncated);
    BOOST_TEST(hex.text.size() <= 4096u);
    BOOST_TEST(hex.text.starts_with("\\x0A\\x0A"));
}

BOOST_FIXTURE_TEST_CASE(reads_scan_the_source_exactly_once, Scratch)
{
    const std::string text = std::string(400000, 'a') + "\r\n" + std::string(200000, 'b');
    const auto path = write(text);
    struct stat source {};
    BOOST_REQUIRE(::stat(path.c_str(), &source) == 0);
    counted_reads.clear();
    counting = true;
    const auto lines = textedit::read_file_lines_bounded(
        path, 0, 3, textedit::LineReadFormat::ByteRange, 4096, text.size() * 2);
    const auto hex = textedit::read_file_bytes_bounded(
        path, 5, 10, textedit::ByteReadFormat::HexEscaped, 40, text.size() * 2);
    counting = false;
    BOOST_TEST(lines.total_bytes == text.size());
    BOOST_TEST(hex.total_bytes == text.size());
    // Two calls, each one full scan of one file, and nothing else.
    BOOST_TEST(counted_reads.size() == 1u);
    BOOST_TEST(counted_reads[source.st_ino] == 2 * text.size());
}
