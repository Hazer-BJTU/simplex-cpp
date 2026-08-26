#include "llm/chat_completions/interpreter.hpp"

#include <string>
#include <utility>

namespace llm::chat_completions {

namespace {

namespace http = boost::beast::http;
using nlohmann::json;

std::string wire_arguments(const json& arguments) {
    return arguments.is_string() ? arguments.get<std::string>()
                                 : arguments.dump();
}

std::string text_content(const std::vector<model_io::Content>& content) {
    std::string result;
    for (const auto& part : content) result += part.raw;
    return result;
}

json user_content_part(const model_io::Content& content) {
    if (content.type == model_io::ContentType::ExternalRef) {
        json image = json::object();
        if (content.extras && content.extras->is_object()) {
            const auto it = content.extras->find("image_url");
            if (it != content.extras->end()) {
                image = it->is_object() ? *it : json{{"url", *it}};
            }
            if (const auto detail = content.extras->find("detail");
                detail != content.extras->end()) {
                image["detail"] = *detail;
            }
        }
        if (!image.contains("url")) image["url"] = content.raw;
        return json{{"type", "image_url"}, {"image_url", std::move(image)}};
    }
    return json{{"type", "text"}, {"text", content.raw}};
}

json user_content(const std::vector<model_io::Content>& content) {
    // The string form is accepted by the broadest set of compatible servers.
    if (content.empty()) return "";
    if (content.size() == 1 &&
        content.front().type == model_io::ContentType::Text) {
        return content.front().raw;
    }
    json::array_t parts;
    parts.reserve(content.size());
    for (const auto& part : content) parts.push_back(user_content_part(part));
    return parts;
}

std::string derived_role(const model_io::MessageItem& item) {
    if (!item.role.empty()) return item.role;
    if (item.type == model_io::MessageItemType::ModelResponse) return "assistant";
    if (item.type == model_io::MessageItemType::InvokeReturn) return "tool";
    return "user";
}

json tool_call(const model_io::InvokeQuery& query) {
    return json{
        {"id", query.id},
        {"type", "function"},
        {"function", {
            {"name", query.name},
            {"arguments", wire_arguments(query.arguments)},
        }},
    };
}

json assistant_message(const model_io::MessageItem& response,
                       bool replay_reasoning) {
    json message{{"role", "assistant"}};
    std::string content;
    std::string refusal;
    for (const auto& part : response.content) {
        std::string part_refusal;
        if (part.extras && part.extras->is_object()) {
            const auto it = part.extras->find("refusal");
            if (it != part.extras->end() && it->is_string()) {
                part_refusal = it->get<std::string>();
                refusal += part_refusal;
            }
        }
        // A refusal-only response keeps the refusal in raw as a readable
        // fallback. Do not replay that fallback as ordinary assistant text.
        if (part_refusal.empty() || part.raw != part_refusal) {
            content += part.raw;
        }
    }
    if (content.empty() &&
        ((!refusal.empty()) ||
         (response.invokes && !response.invokes->empty()))) {
        message["content"] = nullptr;
    } else {
        message["content"] = content;
    }
    if (!refusal.empty()) message["refusal"] = std::move(refusal);
    // Thinking-mode providers (dialect opt-in) require the intermediate
    // reasoning replayed verbatim; strict servers reject the unknown field.
    if (replay_reasoning && response.reasoning &&
        !response.reasoning->raw.empty()) {
        message["reasoning_content"] = response.reasoning->raw;
    }
    if (response.invokes && !response.invokes->empty()) {
        json::array_t calls;
        calls.reserve(response.invokes->size());
        for (const auto& call : *response.invokes) {
            calls.push_back(tool_call(call));
        }
        message["tool_calls"] = std::move(calls);
    }
    return message;
}

void emit_tool_results(
    json::array_t& messages,
    const std::vector<model_io::MessageItem>& results,
    const std::optional<std::vector<model_io::InvokeQuery>>& invokes) {
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& item = results[index];
        json message{{"role", "tool"}, {"content", text_content(item.content)}};
        std::string call_id;
        if (item.invoke_return && !item.invoke_return->query.id.empty()) {
            call_id = item.invoke_return->query.id;
            if (item.content.empty()) {
                message["content"] = item.invoke_return->output.raw;
            }
        } else if (invokes && results.size() == invokes->size() &&
                   index < invokes->size()) {
            call_id = (*invokes)[index].id;
        }
        if (!call_id.empty()) message["tool_call_id"] = std::move(call_id);
        messages.push_back(std::move(message));
    }
}

void emit_message(json::array_t& messages,
                  const model_io::MessageItem& item) {
    if (item.type == model_io::MessageItemType::InvokeReturn) {
        emit_tool_results(messages, {item}, std::nullopt);
        return;
    }
    messages.push_back(json{
        {"role", derived_role(item)},
        {"content", user_content(item.content)},
    });
}

// Translate the shared host config envelope ("reasoning": {"effort": ...},
// the Responses-API spelling) into the chat-completions top-level
// reasoning_effort. An explicit top-level value wins; the envelope object is
// then consumed either way — chat servers reject unknown top-level params,
// so leaving it behind would turn every configured conversation into a 400.
void translate_reasoning_envelope(json& body) {
    const auto envelope = body.find("reasoning");
    if (envelope == body.end()) return;
    if (envelope->is_object()) {
        const auto effort = envelope->find("effort");
        if (effort != envelope->end() && effort->is_string() &&
            !body.contains("reasoning_effort")) {
            body["reasoning_effort"] = *effort;
        }
    }
    body.erase("reasoning");
}

} // namespace

endpoint::ModelRequestInterpreter::HttpRequest
ChatCompletionsInterpreter::build_request(
    const model_io::AgentInputState& conversation,
    const model_io::ModelEndpoint& endpoint,
    const nlohmann::json& generation) {
    const auto model = generation.find("model");
    if (model == generation.end() || !model->is_string() ||
        model->get_ref<const std::string&>().empty()) {
        throw HttpRequestException(
            HttpRequestException::Stage::CreateRequest,
            "generation carries no non-empty \"model\"");
    }
    const endpoint::ResolvedEndpoint where = endpoint::resolve_endpoint(endpoint);

    json body = generation;
    json::array_t messages;
    if (const auto prompt = conversation.system_prompt.render();
        !prompt.markdown.empty()) {
        messages.push_back(
            json{{"role", "system"}, {"content", prompt.markdown}});
    }

    for (const auto& turn : conversation.turns) {
        emit_message(messages, turn.user_input);
        for (const auto& step : turn.agent_loop_step) {
            messages.push_back(assistant_message(
                step.model_response, _dialect->replay_assistant_reasoning()));
            if (step.invoke_returns) {
                emit_tool_results(messages, *step.invoke_returns,
                                  step.model_response.invokes);
            }
        }
    }
    body["messages"] = std::move(messages);

    if (!conversation.tools.empty()) {
        json::array_t tools;
        tools.reserve(conversation.tools.size());
        for (const auto& tool : conversation.tools) {
            json function{
                {"name", tool.name},
                {"parameters", tool.argument_schema},
            };
            if (!tool.description.empty()) {
                function["description"] = tool.description;
            }
            tools.push_back(json{
                {"type", "function"},
                {"function", std::move(function)},
            });
        }
        body["tools"] = std::move(tools);
    } else {
        // Builder-owned: stale generation definitions must not leak through.
        body.erase("tools");
    }

    body["stream"] = true;
    body["n"] = 1; // AgentInputState has one model-response slot per exchange.
    translate_reasoning_envelope(body);
    // The reader's cost accounting lives on the empty-choices usage trailer,
    // so the builder always requests it. Key-level: sibling stream_options
    // survive, and a dialect may still strip the whole object.
    if (!body["stream_options"].is_object()) {
        body["stream_options"] = json::object();
    }
    body["stream_options"]["include_usage"] = true;
    _dialect->transform_request(body);

    HttpRequest request{http::verb::post, where.target, 11};
    request.set(http::field::host, where.authority());
    endpoint::apply_transport_headers(request, endpoint);
    request.set(http::field::accept, "text/event-stream");
    request.set(http::field::content_type, "application/json");
    request.body() = body.dump();
    request.prepare_payload();
    return request;
}

} // namespace llm::chat_completions
