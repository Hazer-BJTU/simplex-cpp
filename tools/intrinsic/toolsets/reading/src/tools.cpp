#include "tools/intrinsic/reading/tools.hpp"
#include "tools/intrinsic/reading/schemas.hpp"
#include "tools/intrinsic/tool_result.hpp"
#include "textedit/read.hpp"
#include "textedit/utf8_probe.hpp"

#include <format>
#include <limits>
#include <stdexcept>

namespace tools::intrinsic {

ReadTextTool::ReadTextTool()
    : DeclaredTool(reading::schema_directory() / "read_text.yaml")
{
    initialize_configuration([this](const nlohmann::json& config) {
        for (const auto& [key, value] : config.items()) {
            if (key != "max_file_bytes" && key != "max_output_bytes") {
                configuration_error(key, "unknown read_text configuration field");
            }
        }
        const auto limit = [this, &config](
            const char* key, std::size_t fallback, std::size_t maximum) {
            const auto found = config.find(key);
            if (found == config.end()) {
                return fallback;
            }
            if (!found->is_number_integer() || *found < 1 || *found > maximum) {
                configuration_error(key, std::format(
                    "must be an integer from 1 to {} bytes", maximum));
            }
            return found->get<std::size_t>();
        };
        const auto file_bytes = limit("max_file_bytes", kMaxFileBytes, kMaxConfiguredFileBytes);
        const auto output_bytes = limit("max_output_bytes", kMaxOutputBytes, kMaxConfiguredOutputBytes);
        max_file_bytes_ = file_bytes;
        max_output_bytes_ = output_bytes;
    });
}

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
        textedit::BoundedReadResult selected;
        if (mode == "lines") {
            const auto layout = format == "line_index" ? textedit::LineReadFormat::LineIndex
                : format == "byte_range" ? textedit::LineReadFormat::ByteRange
                : textedit::LineReadFormat::Plain;
            auto lines = textedit::read_file_lines_bounded(
                path, start, count, layout, max_output_bytes_, max_file_bytes_);
            output.field("lines_read", lines.lines_read);
            selected = std::move(lines);
        } else {
            const auto layout = format == "hex_escaped" ? textedit::ByteReadFormat::HexEscaped
                : textedit::ByteReadFormat::Plain;
            selected = textedit::read_file_bytes_bounded(
                path, start, count, layout, max_output_bytes_, max_file_bytes_);
        }

        // Probe separately: this is advisory evidence, never a read precondition
        // or a guarantee that a concurrently changing file matches the selection.
        const auto probe = textedit::probe_utf8_file(path);
        std::vector<std::string> hints;
        if (probe.likelihood == textedit::Utf8TextLikelihood::Unlikely) {
            hints.emplace_back("File may not be UTF-8 text; use bytes with hex_escaped for exact bytes.");
        }
        if (selected.display_replaced) {
            hints.emplace_back("Invalid UTF-8 bytes replaced for display; use hex_escaped for exact bytes.");
        }
        if (selected.output_truncated) {
            hints.emplace_back(std::format(
                "Output clipped to {} bytes; reduce count and reread. For a single long line, use byte mode.",
                max_output_bytes_));
        }
        output.field("total_lines", selected.total_lines)
            .field("total_bytes", selected.total_bytes)
            .field("reached_end", selected.reached_end)
            .field("output_truncated", selected.output_truncated)
            .field("display_replaced", selected.display_replaced)
            .field("hints", hints)
            .block("text", std::move(selected.text), selected.output_truncated);
        co_return output.render();
    } catch (const std::exception& error) {
        invoke_failed(std::string("read_text: ") + error.what());
    }
}

} // namespace tools::intrinsic
