#define BOOST_TEST_MODULE Utf8Probe
#include <boost/test/unit_test.hpp>

#include "textedit/utf8_probe.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>

BOOST_TEST_DONT_PRINT_LOG_VALUE(textedit::Utf8TextLikelihood)

namespace {

/// Isolated files for real IO tests; clean up even if an assertion throws.
struct Scratch {
    std::filesystem::path root;

    Scratch()
    {
        auto pattern = (std::filesystem::temp_directory_path() /
                        "simplex-utf8-XXXXXX").string();
        const auto directory = ::mkdtemp(pattern.data());
        if (!directory) {
            throw std::runtime_error("cannot create UTF-8 probe test directory");
        }
        root = directory;
    }

    ~Scratch()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    std::filesystem::path write(const std::string& bytes) const
    {
        const auto path = root / "sample";
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        BOOST_REQUIRE(output.good());
        return path;
    }
};

} // namespace

BOOST_FIXTURE_TEST_CASE(accepts_empty_ascii_bom_and_multibyte_text, Scratch)
{
    for (const std::string bytes : {
             "", "hello\tworld\r\n\f", "\xef\xbb\xbftext",
             "\xc2\xa2\xe4\xb8\xad\xf0\x9f\x98\x80",
             "\xe0\xa0\x80\xed\x9f\xbf\xf4\x8f\xbf\xbf"}) {
        const auto result = textedit::probe_utf8_file(write(bytes));
        BOOST_TEST(result.likelihood == textedit::Utf8TextLikelihood::Likely);
        BOOST_TEST(!result.error);
        BOOST_TEST(result.sampled_bytes == bytes.size());
        BOOST_TEST(!result.truncated);
        BOOST_TEST(!result.incomplete_suffix);
        BOOST_TEST(!result.invalid_utf8_offset.has_value());
    }
}

BOOST_FIXTURE_TEST_CASE(rejects_malformed_encodings_as_advice, Scratch)
{
    for (const std::string malformed : {
             "\x80", "\xc0\xaf", "\xc1\xbf", "\xe0\x80\x80",
             "\xed\xa0\x80", "\xf0\x80\x80\x80", "\xf4\x90\x80\x80",
             "\xf5\x80\x80\x80", "\xff", "\xc2x", "\xe2\x82"}) {
        const auto result = textedit::probe_utf8_file(write("ok" + malformed));
        BOOST_TEST(result.likelihood == textedit::Utf8TextLikelihood::Unlikely);
        BOOST_TEST(!result.error);
        BOOST_REQUIRE(result.invalid_utf8_offset.has_value());
        BOOST_TEST(*result.invalid_utf8_offset == 2u);
        BOOST_TEST(!result.incomplete_suffix);
    }
}

BOOST_FIXTURE_TEST_CASE(binary_controls_are_distinct_from_invalid_utf8, Scratch)
{
    const auto result = textedit::probe_utf8_file(write(std::string("a\0b\x01\x7f", 5)));
    BOOST_TEST(result.likelihood == textedit::Utf8TextLikelihood::Unlikely);
    BOOST_TEST(result.suspicious_control_bytes == 3u);
    BOOST_TEST(!result.invalid_utf8_offset.has_value());
    BOOST_TEST(!result.error);

    const auto utf16 = textedit::probe_utf8_file(write(std::string("\xff\xfeh\0", 4)));
    BOOST_TEST(utf16.likelihood == textedit::Utf8TextLikelihood::Unlikely);
}

BOOST_FIXTURE_TEST_CASE(sample_boundary_is_not_a_malformed_end_of_file, Scratch)
{
    for (const std::string codepoint : {"\xc2\xa2", "\xe2\x82\xac", "\xf0\x9f\x98\x80"}) {
        for (std::size_t count = 1; count < codepoint.size(); ++count) {
            const auto result = textedit::probe_utf8_file(write(codepoint), count);
            BOOST_TEST(result.likelihood == textedit::Utf8TextLikelihood::Likely);
            BOOST_TEST(result.sampled_bytes == count);
            BOOST_TEST(result.truncated);
            BOOST_TEST(result.incomplete_suffix);
            BOOST_TEST(!result.invalid_utf8_offset.has_value());

            const auto eof = textedit::probe_utf8_file(write(codepoint.substr(0, count)), count);
            BOOST_TEST(eof.likelihood == textedit::Utf8TextLikelihood::Unlikely);
            BOOST_TEST(!eof.truncated);
            BOOST_TEST(!eof.incomplete_suffix);
        }
    }
    const auto bad_prefix = textedit::probe_utf8_file(write("\xe0\x80\x80"), 2);
    BOOST_TEST(bad_prefix.likelihood == textedit::Utf8TextLikelihood::Unlikely);
    BOOST_TEST(!bad_prefix.incomplete_suffix);
}

BOOST_FIXTURE_TEST_CASE(limits_evidence_to_the_sample_and_does_not_write, Scratch)
{
    const std::string bytes = std::string(8192, 'a') + '\xff';
    const auto path = write(bytes);
    const auto before = std::filesystem::last_write_time(path);
    const auto result = textedit::probe_utf8_file(path);
    BOOST_TEST(result.likelihood == textedit::Utf8TextLikelihood::Likely);
    BOOST_TEST(result.sampled_bytes == 8192u);
    BOOST_TEST(result.truncated);
    BOOST_TEST(!result.incomplete_suffix);
    BOOST_CHECK(std::filesystem::last_write_time(path) == before);
    std::ifstream input(path, std::ios::binary);
    const std::string after{std::istreambuf_iterator<char>(input), {}};
    BOOST_TEST(after == bytes);

    const auto exact = textedit::probe_utf8_file(write("abcd"), 4);
    BOOST_TEST(!exact.truncated);
    const auto larger = textedit::probe_utf8_file(write(bytes), bytes.size());
    BOOST_TEST(larger.likelihood == textedit::Utf8TextLikelihood::Unlikely);
}

BOOST_FIXTURE_TEST_CASE(io_failures_are_unknown_and_symlinks_are_followed, Scratch)
{
    const auto missing = textedit::probe_utf8_file(root / "missing");
    BOOST_TEST(missing.likelihood == textedit::Utf8TextLikelihood::Unknown);
    BOOST_TEST(static_cast<bool>(missing.error));
    const auto directory = textedit::probe_utf8_file(root);
    BOOST_TEST(directory.likelihood == textedit::Utf8TextLikelihood::Unknown);
    BOOST_TEST(static_cast<bool>(directory.error));

    const auto fifo = root / "pipe";
    BOOST_REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    const auto special = textedit::probe_utf8_file(fifo);
    BOOST_TEST(special.likelihood == textedit::Utf8TextLikelihood::Unknown);
    BOOST_TEST(static_cast<bool>(special.error));

    std::filesystem::create_symlink(write("text"), root / "link");
    BOOST_TEST(textedit::probe_utf8_file(root / "link").likelihood ==
               textedit::Utf8TextLikelihood::Likely);
    const auto nul_path = textedit::probe_utf8_file(std::filesystem::path(
        (root / "sample").string() + std::string("\0suffix", 7)));
    BOOST_TEST(nul_path.likelihood == textedit::Utf8TextLikelihood::Unknown);
    BOOST_TEST(nul_path.error == std::make_error_code(std::errc::invalid_argument));
    BOOST_CHECK_THROW((void)textedit::probe_utf8_file(root / "link", 0), std::invalid_argument);
    BOOST_CHECK_THROW((void)textedit::probe_utf8_file(root / "link", 1024 * 1024 + 1),
                      std::invalid_argument);
}
