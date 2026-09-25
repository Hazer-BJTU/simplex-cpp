#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <system_error>

namespace textedit {

/** Advisory classification of a bounded file prefix, never an access policy. */
enum class Utf8TextLikelihood {
    Likely,
    Unlikely,
    Unknown
};

/**
 * Evidence from a UTF-8 text probe. No sampled file contents are retained.
 *
 * Likely means the inspected prefix contains neither malformed UTF-8 nor
 * suspicious control characters. Empty files and ASCII qualify. This does not
 * prove that the rest of the file is UTF-8, or that valid UTF-8 bytes are text.
 * Unlikely means at least one such problem was observed; it is only a hint.
 * Unknown means no assessment is available because the file could not be read.
 */
struct Utf8ProbeResult {
    Utf8TextLikelihood likelihood = Utf8TextLikelihood::Unknown;
    /// Number of prefix bytes inspected; excludes the optional lookahead byte.
    std::size_t sampled_bytes = 0;
    /// Another byte exists beyond the sample, so the file was not fully checked.
    bool truncated = false;
    /// A potentially valid code point crosses the sampling boundary. This alone
    /// does not make the file unlikely to be UTF-8. At EOF it is instead invalid.
    bool incomplete_suffix = false;
    /// Zero-based offset of the first malformed sequence in the sample.
    std::optional<std::size_t> invalid_utf8_offset;
    /// Count of ASCII controls other than TAB, LF, CR and FF (including NUL/DEL).
    /// Even one is a conservative binary-text warning, not an encoding failure.
    std::size_t suspicious_control_bytes = 0;
    /// Open/status/read failure; such failures are not encoding evidence.
    std::error_code error;
};

/**
 * Inspect the beginning of a regular file without modifying it.
 *
 * Reads at most sample_bytes + 1 bytes, with bounded memory and synchronous IO.
 * The extra byte only detects truncation. Strict UTF-8 validation rejects
 * overlong encodings, surrogates and code points above U+10FFFF. A UTF-8 BOM is
 * accepted; other encodings are not converted or positively identified.
 *
 * Relative paths use the process working directory; symlinks are followed.
 * Non-regular files are refused. Concurrent file changes are not a snapshot:
 * this function is advisory and must not be used as write precondition evidence.
 * Operational failures return Unknown with error set. Allocation failures may
 * throw. Zero or more than 1 MiB sample_bytes throws std::invalid_argument.
 * Callers decide whether and how to display a warning; no IO policy is enforced.
 */
[[nodiscard]] Utf8ProbeResult probe_utf8_file(
    const std::filesystem::path& path,
    std::size_t sample_bytes = 8192);

} // namespace textedit
