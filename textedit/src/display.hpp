#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace textedit::detail {

/**
 * Append validated UTF-8 scalars while enforcing an output byte budget.
 *
 * Invalid source bytes each become U+FFFD. The decision is made before any
 * clipping, so a run of continuation bytes cannot make the writer retreat or
 * hide display_replaced. The writer never retains input views: callers may pass
 * any number of separate pieces of one logical stream.
 */
class DisplayWriter {
public:
    explicit DisplayWriter(std::size_t budget) : budget_(budget) {}

    void append(std::string_view source)
    {
        for (std::size_t offset = 0; offset < source.size();) {
            const auto first = static_cast<unsigned char>(source[offset]);
            std::size_t width = 1;
            bool valid = first < 0x80;
            if (!valid) {
                if (first >= 0xc2 && first <= 0xdf) width = 2;
                else if (first >= 0xe0 && first <= 0xef) width = 3;
                else if (first >= 0xf0 && first <= 0xf4) width = 4;
                if (width > 1 && source.size() - offset >= width) {
                    valid = true;
                    for (std::size_t index = 1; index < width; ++index) {
                        const auto byte = static_cast<unsigned char>(source[offset + index]);
                        if (byte < 0x80 || byte > 0xbf ||
                            (index == 1 && first == 0xe0 && byte < 0xa0) ||
                            (index == 1 && first == 0xed && byte > 0x9f) ||
                            (index == 1 && first == 0xf0 && byte < 0x90) ||
                            (index == 1 && first == 0xf4 && byte > 0x8f)) {
                            valid = false;
                            break;
                        }
                    }
                }
            }

            const std::size_t emitted = valid ? width : 3;
            if (emitted > budget_ - output_.size()) {
                truncated_ = true;
                return;
            }
            if (valid) {
                output_.append(source.data() + offset, width);
                offset += width;
            } else {
                output_ += "\xef\xbf\xbd";
                replaced_ = true;
                ++offset;
            }
            consumed_ += valid ? width : 1;
        }
    }

    /** Hex output is ASCII and expands every byte by exactly four bytes. */
    void append_hex(std::string_view source)
    {
        constexpr char digits[] = "0123456789ABCDEF";
        const auto fits = std::min(source.size(), (budget_ - output_.size()) / 4);
        for (std::size_t offset = 0; offset < fits; ++offset) {
            const auto byte = static_cast<unsigned char>(source[offset]);
            output_ += "\\x";
            output_ += digits[byte >> 4];
            output_ += digits[byte & 0x0f];
        }
        truncated_ |= fits != source.size();
        consumed_ += fits;
    }

    [[nodiscard]] bool truncated() const noexcept { return truncated_; }
    [[nodiscard]] bool replaced() const noexcept { return replaced_; }
    /** Source bytes fully consumed by the emitted display text. */
    [[nodiscard]] std::size_t consumed() const noexcept { return consumed_; }
    [[nodiscard]] const std::string& text() const noexcept { return output_; }
    [[nodiscard]] std::string take() { return std::move(output_); }

    /** Mark the result as clipped when the caller withheld source bytes. */
    void mark_truncated() noexcept { truncated_ = true; }

private:
    std::size_t budget_;
    std::string output_;
    std::size_t consumed_ = 0;
    bool truncated_ = false;
    bool replaced_ = false;
};

} // namespace textedit::detail
