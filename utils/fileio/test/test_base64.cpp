#define BOOST_TEST_MODULE FileBase64
#include <boost/test/unit_test.hpp>

#include "fileio/base64.hpp"

#include <openssl/evp.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

struct Scratch {
    fs::path root = fs::temp_directory_path()
        / ("simplex_base64_" + std::to_string(::getpid()));

    Scratch()
    {
        fs::remove_all(root);
        fs::create_directories(root);
    }

    ~Scratch()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    void write(const fs::path& name, std::string_view bytes) const
    {
        std::ofstream output(root / name, std::ios::binary);
        BOOST_REQUIRE(output.good());
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        BOOST_REQUIRE(output.good());
    }

    std::string read(const fs::path& name) const
    {
        std::ifstream input(root / name, std::ios::binary);
        BOOST_REQUIRE(input.good());
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
};

// OpenSSL provides an independent encoding oracle for file-format fixtures.
std::string reference_encode(std::string_view source)
{
    // EVP_EncodeBlock also writes a trailing NUL after the encoded bytes.
    std::string result(4 * ((source.size() + 2) / 3) + 1, '\0');
    const int written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(result.data()),
        reinterpret_cast<const unsigned char*>(source.data()),
        static_cast<int>(source.size()));
    BOOST_REQUIRE(written >= 0);
    result.resize(static_cast<std::size_t>(written));
    return result;
}

} // namespace

BOOST_AUTO_TEST_CASE(rfc_4648_vectors_and_arbitrary_octets)
{
    const std::array<std::pair<std::string_view, std::string_view>, 7> vectors {{
        {"", ""},
        {"f", "Zg=="},
        {"fo", "Zm8="},
        {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="},
        {"fooba", "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"},
    }};
    for (const auto& [plain, encoded] : vectors) {
        BOOST_TEST(fileio::base64_encode(plain) == encoded);
        BOOST_TEST(fileio::base64_decode(encoded) == plain);
    }

    std::string binary;
    for (int value = 0; value < 256; ++value) {
        binary.push_back(static_cast<char>(value));
    }
    BOOST_TEST(fileio::base64_encode(binary) == reference_encode(binary));
    BOOST_TEST(fileio::base64_decode(reference_encode(binary)) == binary);
}

BOOST_AUTO_TEST_CASE(common_file_formats_survive_file_to_base64_to_file)
{
    Scratch scratch;
    const std::array<std::string_view, 5> fixtures {{
        "pixel.png", "icon.gif", "report.pdf", "archive.zip", "note.txt",
    }};
    for (const auto filename : fixtures) {
        const auto input = fs::path(TEST_BASE64_FIXTURE_DIR) / filename;
        const std::string content = scratch.read(input);
        const auto output = scratch.root / (std::string(filename) + ".copy");

        const std::string encoded = fileio::base64_encode_file(input);
        BOOST_TEST(encoded == reference_encode(content));
        fileio::base64_decode_file(encoded, output);
        BOOST_TEST(scratch.read(output.filename()) == content);
    }
}

BOOST_AUTO_TEST_CASE(large_binary_and_empty_files)
{
    Scratch scratch;
    std::string content(40001, '\0');
    for (std::size_t index = 0; index < content.size(); ++index) {
        content[index] = static_cast<char>(index % 256);
    }
    scratch.write("large.bin", content);
    const auto encoded = fileio::base64_encode_file(scratch.root / "large.bin");
    BOOST_TEST(encoded == reference_encode(content));
    fileio::base64_decode_file(encoded, scratch.root / "large.copy");
    BOOST_TEST(scratch.read("large.copy") == content);

    scratch.write("empty.bin", "");
    BOOST_TEST(fileio::base64_encode_file(scratch.root / "empty.bin").empty());
    fileio::base64_decode_file("", scratch.root / "empty.copy");
    BOOST_TEST(scratch.read("empty.copy").empty());
}

BOOST_AUTO_TEST_CASE(invalid_encoding_never_replaces_destination)
{
    Scratch scratch;
    scratch.write("original", "keep this file");
    const std::array<std::string_view, 12> invalid {{
        "A", "AAA", "A===", "====", "AA=A", "AA==AA==",
        "Zg=", "Zh==", "Zm9=", "Zm9v\n", "Zm9_", "Zm9-",
    }};
    for (const auto encoded : invalid) {
        BOOST_CHECK_THROW((void)fileio::base64_decode(encoded), std::invalid_argument);
        BOOST_CHECK_THROW(
            fileio::base64_decode_file(encoded, scratch.root / "original"),
            std::invalid_argument);
        BOOST_TEST(scratch.read("original") == "keep this file");
    }

    BOOST_CHECK_THROW(
        (void)fileio::base64_encode_file(scratch.root / "missing"), std::system_error);
    BOOST_CHECK_THROW(
        (void)fileio::base64_encode_file(scratch.root), std::system_error);
}
