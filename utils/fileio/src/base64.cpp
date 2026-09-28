#include "fileio/base64.hpp"

#include "fileio/atomic_write.hpp"
#include "fileio/read_prefix.hpp"

#include <limits>
#include <ostream>
#include <stdexcept>

namespace fileio {
namespace {

constexpr std::string_view alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int digit(char character) noexcept
{
    if (character >= 'A' && character <= 'Z') {
        return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
        return character - '0' + 52;
    }
    if (character == '+') {
        return 62;
    }
    if (character == '/') {
        return 63;
    }
    return -1;
}

int required_digit(char character)
{
    const int value = digit(character);
    if (value < 0) {
        throw std::invalid_argument("invalid Base64 character or padding");
    }
    return value;
}

} // namespace

std::string base64_encode(std::string_view bytes)
{
    const std::size_t groups = bytes.size() / 3;
    const std::size_t tail = bytes.size() % 3;
    const std::size_t padding_group = tail == 0 ? 0 : 4;
    std::string encoded;
    if (groups > (encoded.max_size() - padding_group) / 4) {
        throw std::length_error("Base64 output exceeds maximum string size");
    }
    encoded.reserve(groups * 4 + padding_group);

    for (std::size_t index = 0; index < groups * 3; index += 3) {
        const auto first = static_cast<unsigned char>(bytes[index]);
        const auto second = static_cast<unsigned char>(bytes[index + 1]);
        const auto third = static_cast<unsigned char>(bytes[index + 2]);
        encoded.push_back(alphabet[first >> 2]);
        encoded.push_back(alphabet[((first & 0x03) << 4) | (second >> 4)]);
        encoded.push_back(alphabet[((second & 0x0f) << 2) | (third >> 6)]);
        encoded.push_back(alphabet[third & 0x3f]);
    }

    if (tail != 0) {
        const auto first = static_cast<unsigned char>(bytes[groups * 3]);
        encoded.push_back(alphabet[first >> 2]);
        if (tail == 1) {
            encoded.push_back(alphabet[(first & 0x03) << 4]);
            encoded.append("==");
        } else {
            const auto second = static_cast<unsigned char>(bytes[groups * 3 + 1]);
            encoded.push_back(alphabet[((first & 0x03) << 4) | (second >> 4)]);
            encoded.push_back(alphabet[(second & 0x0f) << 2]);
            encoded.push_back('=');
        }
    }
    return encoded;
}

std::string base64_decode(std::string_view encoded)
{
    if (encoded.size() % 4 != 0) {
        throw std::invalid_argument("Base64 length must be a multiple of four");
    }

    std::string bytes;
    bytes.reserve((encoded.size() / 4) * 3);
    for (std::size_t index = 0; index < encoded.size(); index += 4) {
        const int first = required_digit(encoded[index]);
        const int second = required_digit(encoded[index + 1]);
        const bool third_padding = encoded[index + 2] == '=';
        const bool fourth_padding = encoded[index + 3] == '=';
        if ((third_padding || fourth_padding) && index + 4 != encoded.size()) {
            throw std::invalid_argument("Base64 padding must end the input");
        }

        if (third_padding) {
            if (!fourth_padding || (second & 0x0f) != 0) {
                throw std::invalid_argument("invalid Base64 padding bits");
            }
            bytes.push_back(static_cast<char>((first << 2) | (second >> 4)));
            continue;
        }

        const int third = required_digit(encoded[index + 2]);
        bytes.push_back(static_cast<char>((first << 2) | (second >> 4)));
        bytes.push_back(static_cast<char>((second << 4) | (third >> 2)));
        if (fourth_padding) {
            if ((third & 0x03) != 0) {
                throw std::invalid_argument("invalid Base64 padding bits");
            }
            continue;
        }

        const int fourth = required_digit(encoded[index + 3]);
        bytes.push_back(static_cast<char>((third << 6) | fourth));
    }
    return bytes;
}

std::string base64_encode_file(const std::filesystem::path& path)
{
    return base64_encode(read_prefix(path, std::numeric_limits<std::size_t>::max()));
}

void base64_decode_file(std::string_view encoded, const std::filesystem::path& path)
{
    const std::string bytes = base64_decode(encoded);
    atomic_write(path, [&](std::ostream& output) {
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    });
}

} // namespace fileio
