#include "tools/intrinsic/tool_result.hpp"
#include "textformat/document.hpp"

#include <algorithm>
#include <format>

namespace tools::intrinsic {

ToolResult& ToolResult::field(std::string_view name, nlohmann::json value) {
    if (value.is_null()
        || ((value.is_array() || value.is_object()) && value.empty())) {
        return *this;
    }
    if (value.is_string() && value.get_ref<const std::string&>().empty()) {
        return *this;
    }
    std::string rendered;
    if (value.is_string()) {
        const auto& raw = value.get_ref<const std::string&>();
        const bool controls = std::any_of(raw.begin(), raw.end(), [](unsigned char c) {
            return c < 0x20 || c == 0x7f;
        });
        rendered = controls ? value.dump() : raw;
    } else {
        rendered = value.dump();
    }
    auto& metadata = _records.back().metadata;
    metadata += textformat::metadata_field(name, rendered);
    _dirty = true;
    return *this;
}

ToolResult& ToolResult::block(std::string_view name, std::string body, bool truncated) {
    auto& output = _records.back().output;
    if (!output.empty()) {
        output += '\n';
    }
    output += name;
    if (body.empty()) {
        output += truncated ? ": (empty, truncated)\n" : ": (empty)\n";
    } else {
        output += std::format(" ({}{} bytes):\n",
            truncated ? "truncated, first " : "", body.size());
        output += body;
        if (body.back() != '\n') {
            output += '\n';
        }
    }
    _dirty = true;
    return *this;
}

ToolResult& ToolResult::separate() {
    if (!_records.back().metadata.empty() || !_records.back().output.empty()) {
        _records.emplace_back();
        _dirty = true;
    }
    return *this;
}

const std::string& ToolResult::text() const {
    if (!_dirty) {
        return _text;
    }
    std::string rendered;
    for (const auto& record : _records) {
        if (record.metadata.empty() && record.output.empty()) {
            continue;
        }
        if (!rendered.empty()) {
            rendered += "\n---\n\n";
        }
        rendered += record.metadata;
        if (!record.output.empty()) {
            if (!record.metadata.empty()) {
                rendered += '\n';
            }
            rendered += record.output;
        }
    }
    _text = std::move(rendered);
    _dirty = false;
    return _text;
}

bool ToolResult::empty() const noexcept {
    return std::all_of(_records.begin(), _records.end(), [](const Record& record) {
        return record.metadata.empty() && record.output.empty();
    });
}

model_io::Content ToolResult::render() const {
    return model_io::Content{
        .type = model_io::ContentType::Text,
        .raw = text(),
        .extras = std::nullopt,
    };
}

} // namespace tools::intrinsic
