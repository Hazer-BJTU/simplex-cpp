#include "tools/intrinsic/modality_assist/tools.hpp"
#include "tools/intrinsic/modality_assist/schemas.hpp"
#include "tools/intrinsic/tool_result.hpp"
#include "fileio/base64.hpp"
#include "fileio/read_prefix.hpp"

#include <boost/asio/this_coro.hpp>
#include <stdexcept>

namespace tools::intrinsic {
namespace {

/** Suffix-based admission and MIME hint, not image decoding or verification. */
std::string_view image_media_type(const std::filesystem::path& path)
{
    auto extension = path.extension().string();
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
    const auto path = require_string(query, "path", "the local image to interpret");
    if (path.find('\0') != std::string::npos) {
        bad_argument("path must not contain NUL");
    }
    if (image_media_type(path).empty()) {
        bad_argument("path must end in .png, .jpg, .jpeg, .gif, or .webp");
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
    query.type = model_io::InvokeType::ReadOnly;
    query.security = model_io::InvokeSecurity::Trusted;
}

boost::asio::awaitable<model_io::Content> ModalityAssistTool::invoke(
    const model_io::InvokeQuery& query)
{
    co_await boost::asio::this_coro::reset_cancellation_state(
        boost::asio::disable_cancellation());
    const auto path = require_string(query, "path", "the local image to interpret");
    const auto media_type = image_media_type(path);
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
        {
            // Inspect the opened descriptor through read_prefix; no check/open race,
            // no waiting for a FIFO writer. Read one extra byte to reject oversize
            // input without encoding a silently truncated image.
            const auto bytes = fileio::read_prefix(path, kMaxFileBytes + 1);
            file_bytes = bytes.size();
            if (bytes.empty()) throw std::runtime_error("image file is empty");
            if (bytes.size() > kMaxFileBytes) {
                throw std::runtime_error("image file exceeds 16 MiB");
            }
            model_io::Content image;
            image.type = model_io::ContentType::ExternalRef;
            image.modality = model_io::Modality::Image;
            image.raw = "data:" + std::string(media_type) + ";base64,";
            image.raw += fileio::base64_encode(bytes);
            user.content.push_back(std::move(image));
        }

        // Transfer the temporary state rather than copying image data. converse
        // is one exchange; provider retry policy owns any transport retries.
        const auto response = co_await model_->converse(std::move(input));
        ToolResult output;
        output.field("path", path)
            .field("media_type", media_type)
            .field("file_bytes", file_bytes);
        if (response.cost) output.field("token_cost", *response.cost);
        output.block("description", response_text(response));
        co_return output.render();
    } catch (const std::exception& error) {
        invoke_failed(std::string("modality_assist: ") + error.what());
    }
}

} // namespace tools::intrinsic
