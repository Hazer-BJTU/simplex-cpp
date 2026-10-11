#pragma once
#include <string>
#include <string_view>

namespace tools::intrinsic::detail {

/// Text ready for publication and the number of source bytes it represents.
struct TextPrefix {
    std::string text;
    std::size_t consumed = 0;
};

/// Project raw bytes at the model boundary, preserving all valid UTF-8 and
/// controls. Invalid bytes become U+FFFD, one replacement per byte. A valid
/// incomplete suffix stays unread while capture can grow; at EOF/truncation
/// it is replaced. Cursors count original bytes, never rendered UTF-8 bytes.
inline TextPrefix text_prefix(std::string_view raw, bool final)
{
    TextPrefix result;
    while (result.consumed < raw.size()) {
        const auto from = result.consumed;
        const auto first = static_cast<unsigned char>(raw[from]);
        std::size_t length = 0;
        if (first < 0x80) {
            length = 1;
        } else if (first >= 0xc2 && first <= 0xdf) {
            length = 2;
        } else if (first >= 0xe0 && first <= 0xef) {
            length = 3;
        } else if (first >= 0xf0 && first <= 0xf4) {
            length = 4;
        }
        bool valid = length != 0;
        for (std::size_t i = 1; valid && i < length && from + i < raw.size(); ++i) {
            const auto next = static_cast<unsigned char>(raw[from + i]);
            valid = next >= 0x80 && next <= 0xbf;
            if (i == 1) {
                if (first == 0xe0 && next < 0xa0) valid = false;
                if (first == 0xed && next > 0x9f) valid = false;
                if (first == 0xf0 && next < 0x90) valid = false;
                if (first == 0xf4 && next > 0x8f) valid = false;
            }
        }
        if (valid && raw.size() - from < length && !final) break;
        if (!valid || raw.size() - from < length) {
            result.text += "\xef\xbf\xbd";
            ++result.consumed;
        } else {
            result.text.append(raw.substr(from, length));
            result.consumed += length;
        }
    }
    return result;
}
} // namespace tools::intrinsic::detail
