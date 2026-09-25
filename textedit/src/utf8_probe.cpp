#include "textedit/utf8_probe.hpp"

#include "fileio/read_prefix.hpp"
#include <stdexcept>
#include <string>
#include <string_view>

namespace textedit {
namespace {

/// Inspect controls independently so a malformed sequence cannot hide them.
void inspect_prefix(std::string_view bytes, Utf8ProbeResult& result)
{
    for (unsigned char byte : bytes) {
        if ((byte < 0x20 && byte != '\t' && byte != '\n' &&
             byte != '\r' && byte != '\f') || byte == 0x7f) {
            ++result.suspicious_control_bytes;
        }
    }

    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto first = static_cast<unsigned char>(bytes[offset]);
        if (first < 0x80) {
            ++offset;
            continue;
        }

        std::size_t width = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            width = 2;
        } else if (first >= 0xe0 && first <= 0xef) {
            width = 3;
        } else if (first >= 0xf0 && first <= 0xf4) {
            width = 4;
        } else {
            result.invalid_utf8_offset = offset;
            break;
        }

        // Validate available bytes before treating an unfinished suffix as
        // inconclusive: E0 80 at a sample boundary is already invalid.
        bool invalid = false;
        for (std::size_t index = 1;
             index < width && offset + index < bytes.size();
             ++index) {
            const auto byte = static_cast<unsigned char>(bytes[offset + index]);
            if (byte < 0x80 || byte > 0xbf ||
                (index == 1 && first == 0xe0 && byte < 0xa0) ||
                (index == 1 && first == 0xed && byte > 0x9f) ||
                (index == 1 && first == 0xf0 && byte < 0x90) ||
                (index == 1 && first == 0xf4 && byte > 0x8f)) {
                invalid = true;
                break;
            }
        }
        if (invalid) {
            result.invalid_utf8_offset = offset;
            break;
        }
        if (bytes.size() - offset < width) {
            if (result.truncated) {
                result.incomplete_suffix = true;
            } else {
                result.invalid_utf8_offset = offset;
            }
            break;
        }
        offset += width;
    }

    result.likelihood = result.invalid_utf8_offset || result.suspicious_control_bytes
        ? Utf8TextLikelihood::Unlikely
        : Utf8TextLikelihood::Likely;
}

} // namespace

Utf8ProbeResult probe_utf8_file(
    const std::filesystem::path& path,
    std::size_t sample_bytes)
{
    if (sample_bytes == 0 || sample_bytes > 1024 * 1024) {
        throw std::invalid_argument("UTF-8 sample size must be between 1 and 1048576 bytes");
    }

    Utf8ProbeResult result;
    if (path.native().find('\0') != std::string::npos) {
        result.error = std::make_error_code(std::errc::invalid_argument);
        return result;
    }
    std::string bytes;
    try {
        bytes = fileio::read_prefix(path, sample_bytes + 1);
    } catch (const std::system_error& error) {
        result.error = error.code();
        return result;
    }
    const auto count = bytes.size();
    result.truncated = count > sample_bytes;
    result.sampled_bytes = result.truncated ? sample_bytes : count;
    bytes.resize(result.sampled_bytes);
    inspect_prefix(bytes, result);
    return result;
}

} // namespace textedit
