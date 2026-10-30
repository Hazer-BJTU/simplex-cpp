#include "tools/intrinsic/editing/tools.hpp"

#include "textedit/edit.hpp"
#include "tools/intrinsic/editing/schemas.hpp"
#include "tools/intrinsic/tool_result.hpp"

#include <format>
#include <stdexcept>
#include <string>
#include <utility>

namespace tools::intrinsic {

StrReplaceEditTool::StrReplaceEditTool(std::shared_ptr<FileWork> work)
    : DeclaredTool(editing::schema_directory() / "str_replace_edit.yaml"),
      work_(std::move(work))
{
    if (!work_) throw std::invalid_argument("file work executor is required");
    initialize_configuration([this](const nlohmann::json& config) {
        for (const auto& [key, value] : config.items()) {
            if (key != "max_file_bytes") {
                configuration_error(key, "unknown str_replace_edit configuration field");
            }
        }
        const auto found = config.find("max_file_bytes");
        if (found == config.end()) {
            max_file_bytes_ = kMaxFileBytes;
            return;
        }
        if (!found->is_number_integer() || *found < 1 || *found > kMaxConfiguredFileBytes) {
            configuration_error("max_file_bytes", std::format(
                "must be an integer from 1 to {} bytes", kMaxConfiguredFileBytes));
        }
        max_file_bytes_ = found->get<std::size_t>();
    });
}

void StrReplaceEditTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    const auto path = require_string(query, "path", "the existing file to edit");
    if (path.find('\0') != std::string::npos) {
        bad_argument("path must not contain NUL");
    }
    (void)require_string(query, "old_text", "the exact text to replace");
    const auto replacement = find_argument(query, "new_text");
    if (replacement == nullptr || !replacement->is_string()) {
        bad_argument("new_text is required and must be a string; empty string deletes the match");
    }
    if (settle_uint(query, "context_lines", kDefaultContextLines) > kMaxContextLines) {
        bad_argument("context_lines must be between 0 and 20");
    }
}

void StrReplaceEditTool::write_attributes(model_io::InvokeQuery& query) const
{
    query.type = model_io::InvokeType::SerialWrite;
    query.security = model_io::InvokeSecurity::RequireConfirm;
}

boost::asio::awaitable<model_io::Content> StrReplaceEditTool::invoke(
    const model_io::InvokeQuery& query)
{
    const auto path = require_string(query, "path", "the existing file to edit");
    const auto old_text = require_string(query, "old_text", "the exact text to replace");
    const auto new_text = optional_string(query, "new_text");
    const auto context = static_cast<std::size_t>(
        optional_uint(query, "context_lines", kDefaultContextLines));

    textedit::EditResult edit;
    try {
        const auto file_limit = max_file_bytes_;
        edit = co_await work_->run(
            [path, old_text, new_text, context, file_limit](std::stop_token stop) {
                return textedit::str_replace_file(
                    path, old_text, new_text, context, file_limit, stop);
            });
    } catch (const std::exception& error) {
        invoke_failed(std::string("str_replace_edit: ") + error.what());
    }

    if (edit.status == textedit::EditStatus::NotFound) {
        invoke_failed("old_text was not found; read the file and retry with exact bytes");
    }
    if (edit.status == textedit::EditStatus::Ambiguous) {
        invoke_failed("old_text matches at least twice; include more context to make it unique");
    }
    if (edit.status == textedit::EditStatus::Conflict) {
        invoke_failed("file changed during the edit operation; read it again before retrying");
    }

    ToolResult output;
    output.field("status", std::string(textedit::edit_status_name(edit.status)))
        .field("match_line", edit.before_position.line)
        .field("match_byte_column", edit.before_position.column)
        .field("before_bytes", edit.before_bytes)
        .field("after_bytes", edit.after_bytes)
        .field("preview_truncated", edit.preview_truncated);
    if (edit.status == textedit::EditStatus::PublishedSyncFailed) {
        output.field("persistence_error", edit.persistence_error.message())
            .field("hint", "New content is visible but directory durability is uncertain; inspect the file before retrying.");
    }
    output.block("before", std::move(edit.before_excerpt))
        .block("after", std::move(edit.after_excerpt));
    co_return output.render();
}

} // namespace tools::intrinsic
