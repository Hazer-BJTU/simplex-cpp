#include "tools/intrinsic/modality_assist/tools.hpp"
#include "tools/intrinsic/modality_assist/schemas.hpp"
#include "tools/intrinsic/tool_result.hpp"
#include "fileio/base64.hpp"
#include "fileio/read_prefix.hpp"

#include <boost/asio/this_coro.hpp>
#include <format>
#include <stdexcept>

namespace tools::intrinsic {
namespace {

/** Match the image signatures in the WHATWG MIME Sniffing Standard. */
std::string_view header_media_type(std::string_view bytes)
{
    if (bytes.starts_with("\x89PNG\r\n\x1a\n")) return "image/png";
    if (bytes.starts_with("\xff\xd8\xff")) return "image/jpeg";
    if (bytes.starts_with("GIF87a") || bytes.starts_with("GIF89a")) return "image/gif";
    // RIFF is shared with WAV and AVI. Skip its four-byte size field.
    if (bytes.starts_with("RIFF") && bytes.size() >= 14 && bytes.substr(8, 6) == "WEBPVP") {
        return "image/webp";
    }
    return {};
}

std::string_view suffix_media_type(std::string extension)
{
    for (auto& character : extension) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".gif") return "image/gif";
    if (extension == ".webp") return "image/webp";
    return {};
}

/** Use the bytes that will be sent. Keep diagnostics factual even if a later file fails. */
std::string_view select_media_type(const std::filesystem::path& path, std::string_view bytes,
                                   std::size_t index, std::vector<std::string>& hints)
{
    const auto extension = path.extension().string();
    const auto suffix = suffix_media_type(extension);
    const auto header = header_media_type(bytes);
    if (!header.empty()) {
        if (!extension.empty() && header != suffix) {
            if (suffix.empty()) {
                hints.push_back(std::format(
                    "path[{}]: unrecognized filename suffix; the header identifies {}.", index, header));
            } else {
                hints.push_back(std::format(
                    "path[{}]: {} suggests {}, but the header identifies {}.", index, extension, suffix, header));
            }
        }
        return header;
    }
    if (suffix.empty()) {
        throw std::runtime_error("cannot identify PNG, JPEG, GIF, or WebP from the header or filename suffix");
    }
    hints.push_back(std::format(
        "path[{}]: unrecognized image header; inferred {} from {}.", index, suffix, extension));
    return suffix;
}

/** Refuse unsupported responses rather than interpreting tool calls or binary as text. */
std::string response_text(const model_io::MessageItem& response)
{
    if (response.invokes && !response.invokes->empty()) {
        throw std::runtime_error("auxiliary model returned tool calls; no tools were executed");
    }
    std::string text;
    for (const auto& part : response.content) {
        if (part.type != model_io::ContentType::Text || part.modality != model_io::Modality::Text) {
            throw std::runtime_error("auxiliary model returned non-text content");
        }
        if (part.raw.empty()) continue;
        if (!text.empty()) text += '\n';
        text += part.raw;
    }
    if (text.empty()) {
        throw std::runtime_error("auxiliary model returned no textual description");
    }
    return text;
}

} // namespace

ModalityAssistTool::ModalityAssistTool(std::shared_ptr<llm::LLMModel> model)
    : DeclaredTool(modality_assist::schema_directory() / "modality_assist.yaml"),
      model_(std::move(model))
{
    if (!model_ || model_->model_type() != llm::LLMModelType::Conversation) {
        throw std::invalid_argument("modality_assist requires a conversation model");
    }
}

void ModalityAssistTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    const auto paths = optional_string_list(query, "path");
    if (paths.empty()) {
        bad_argument("path must be a nonempty array of image paths");
    }
    for (std::size_t index = 0; index < paths.size(); ++index) {
        const auto label = "path[" + std::to_string(index) + "]";
        if (paths[index].empty() || paths[index].find('\0') != std::string::npos) {
            bad_argument(label + " must be nonempty and must not contain NUL");
        }
    }
    if (settle_string(query, "extra_modality", "vision") != "vision") {
        bad_argument("extra_modality must be vision");
    }
    (void)require_string(query, "request", "the question or description request");
    if (settle_string(query, "system_prompt", kDefaultSystemPrompt).empty()) {
        bad_argument("system_prompt must be nonempty");
    }
}

void ModalityAssistTool::write_attributes(model_io::InvokeQuery& query) const
{
    query.type = model_io::InvokeType::ParallWrite;
    query.security = model_io::InvokeSecurity::Trusted;
}

boost::asio::awaitable<model_io::Content> ModalityAssistTool::invoke(
    const model_io::InvokeQuery& query)
{
    co_await boost::asio::this_coro::reset_cancellation_state(
        boost::asio::disable_cancellation());
    const auto paths = optional_string_list(query, "path");
    std::vector<std::string> hints;
    try {
        model_io::AgentInputState input;
        input.system_prompt.add_section("instruction", "",
            optional_string(query, "system_prompt", kDefaultSystemPrompt));
        auto& user = input.turns.emplace_back().user_input;
        user.role = "user";
        model_io::Content request;
        request.raw = require_string(query, "request", "the question or description request");
        user.content.push_back(std::move(request));

        std::size_t file_bytes = 0;
        std::vector<std::string> media_types;
        for (std::size_t index = 0; index < paths.size(); ++index) {
            try {
                // Bound the whole request, reading one extra byte to detect overflow.
                // Every file must be ready before any model request is submitted.
                const auto remaining = kMaxInputBytes - file_bytes;
                const auto bytes = fileio::read_prefix(paths[index], remaining + 1);
                if (bytes.empty()) throw std::runtime_error("image file is empty");
                if (bytes.size() > remaining) {
                    throw std::runtime_error("combined image size exceeds 16 MiB");
                }
                file_bytes += bytes.size();
                media_types.emplace_back(select_media_type(paths[index], bytes, index, hints));
                model_io::Content image;
                image.type = model_io::ContentType::ExternalRef;
                image.modality = model_io::Modality::Image;
                image.raw = "data:" + media_types.back() + ";base64,";
                image.raw += fileio::base64_encode(bytes);
                user.content.push_back(std::move(image));
            } catch (const std::exception& error) {
                throw std::runtime_error("path[" + std::to_string(index) + "]: " + error.what());
            }
        }

        // Transfer the temporary state rather than copying image data. converse
        // is one exchange; provider retry policy owns any transport retries.
        const auto response = co_await model_->converse(std::move(input));
        ToolResult output;
        output.field("path", paths)
            .field("media_type", media_types)
            .field("file_bytes", file_bytes)
            .field("hints", hints);
        if (response.cost) output.field("token_cost", *response.cost);
        output.block("description", response_text(response));
        co_return output.render();
    } catch (const std::exception& error) {
        std::string message = std::string("modality_assist: ") + error.what();
        if (!hints.empty()) {
            message += "\n" + ToolResult{}.field("hints", hints).text();
        }
        invoke_failed(std::move(message));
    }
}

} // namespace tools::intrinsic
