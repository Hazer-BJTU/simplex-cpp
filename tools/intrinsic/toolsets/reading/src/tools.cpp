#include "tools/intrinsic/reading/tools.hpp"
#include "tools/intrinsic/reading/schemas.hpp"
#include "tools/intrinsic/tool_result.hpp"
#include "textedit/read.hpp"
#include "textedit/utf8_probe.hpp"

#include <limits>
#include <stdexcept>

namespace tools::intrinsic {
namespace {

/// Clip at a UTF-8 boundary. Invalid source bytes are repaired separately.
bool clip_output(std::string& text, std::size_t limit)
{
    if (text.size() <= limit) {
        return false;
    }
    auto end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) {
        --end;
    }
    text.resize(end);
    return true;
}

} // namespace

ReadTextTool::ReadTextTool()
    : DeclaredTool(reading::schema_directory() / "read_text.yaml")
{}

void ReadTextTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    const auto path = require_string(query, "path", "the file to read");
    if (path.find('\0') != std::string::npos) {
        bad_argument("path must not contain NUL");
    }
    const auto mode = settle_string(query, "mode", "lines");
    const auto format = settle_string(query, "format", "plain");
    if (mode != "lines" && mode != "bytes") {
        bad_argument("mode must be lines or bytes");
    }
    if (format != "plain" &&
        !(mode == "lines" && (format == "line_index" || format == "byte_range")) &&
        !(mode == "bytes" && format == "hex_escaped")) {
        bad_argument("format must be plain/line_index/byte_range for lines, or plain/hex_escaped for bytes");
    }
    const auto start = settle_uint(query, "start", 0);
    const auto count = settle_uint(query, "count", kDefaultCount);
    if (start > std::numeric_limits<std::size_t>::max() ||
        count > std::numeric_limits<std::size_t>::max()) {
        bad_argument("start and count must fit a platform byte index");
    }
}

void ReadTextTool::write_attributes(model_io::InvokeQuery& query) const
{
    query.type = model_io::InvokeType::ReadOnly;
    query.security = model_io::InvokeSecurity::Trusted;
}

boost::asio::awaitable<model_io::Content> ReadTextTool::invoke(
    const model_io::InvokeQuery& query)
{
    const auto path = require_string(query, "path", "the file to read");
    const auto mode = optional_string(query, "mode", "lines");
    const auto format = optional_string(query, "format", "plain");
    const auto start = static_cast<std::size_t>(optional_uint(query, "start", 0));
    const auto count = static_cast<std::size_t>(optional_uint(query, "count", kDefaultCount));
    try {
        ToolResult output;
        output.field("path", path);
        textedit::ReadResult selected;
        if (mode == "lines") {
            const auto layout = format == "line_index" ? textedit::LineReadFormat::LineIndex
                : format == "byte_range" ? textedit::LineReadFormat::ByteRange
                : textedit::LineReadFormat::Plain;
            auto lines = textedit::read_file_lines(path, start, count, layout, kMaxFileBytes);
            output.field("lines_read", lines.lines_read);
            selected = std::move(lines);
        } else {
            const auto layout = format == "hex_escaped" ? textedit::ByteReadFormat::HexEscaped
                : textedit::ByteReadFormat::Plain;
            selected = textedit::read_file_bytes(path, start, count, layout, kMaxFileBytes);
        }

        // Probe separately: this is advisory evidence, never a read precondition
        // or a guarantee that a concurrently changing file matches the selection.
        const auto probe = textedit::probe_utf8_file(path);
        std::vector<std::string> hints;
        if (probe.likelihood == textedit::Utf8TextLikelihood::Unlikely) {
            hints.emplace_back("File may not be UTF-8 text; use bytes with hex_escaped for exact bytes.");
        }
        bool truncated = clip_output(selected.text, kMaxOutputBytes);
        // Model messages must serialize as UTF-8 even for malformed input or a
        // byte selection cutting through a character. The library stays lossless.
        auto safe = nlohmann::json::parse(nlohmann::json(selected.text).dump(
            -1, ' ', false, nlohmann::json::error_handler_t::replace)).get<std::string>();
        const bool replaced = safe != selected.text;
        truncated = clip_output(safe, kMaxOutputBytes) || truncated;
        if (replaced) {
            hints.emplace_back("Invalid UTF-8 bytes replaced for display; use hex_escaped for exact bytes.");
        }
        if (truncated) {
            hints.emplace_back("Output clipped to 65536 bytes; reduce count and reread. For a single long line, use byte mode.");
        }
        output.field("total_lines", selected.total_lines)
            .field("total_bytes", selected.total_bytes)
            .field("reached_end", selected.reached_end)
            .field("output_truncated", truncated)
            .field("display_replaced", replaced)
            .field("hints", hints)
            .block("text", std::move(safe), truncated);
        co_return output.render();
    } catch (const std::exception& error) {
        invoke_failed(std::string("read_text: ") + error.what());
    }
}

} // namespace tools::intrinsic
