#include "llm/compat/chat_completions/interpreter.hpp"

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

/**
 * Check one part against the USER-MESSAGE support matrix and explain the refusal.
 *
 * The matrix is (modality, encoding), not modality alone: a part only reaches
 * the wire when the field it maps to can actually carry `raw`. A base64 image
 * has no media type that would let us build an `image_url` from it, and a
 * base64 text part would put the encoding in the message instead of the text.
 * Both would produce a syntactically valid request that means something else,
 * which is worse than a construction error. This is the module's hard error #3
 * (see the header contract).
 *
 * | modality | encoding     | wire                                  |
 * | -------- | ------------ | ------------------------------------- |
 * | text     | text         | `{"type":"text","text":raw}`          |
 * | text     | external_ref | same; the reference travels as text   |
 * | image    | external_ref | `image_url` (URL from `raw`)          |
 * | anything else            | refused                               |
 */
void require_input_part(const model_io::Content& part) {
    const auto modality = part.modality;
    if (modality == model_io::Modality::Audio ||
        modality == model_io::Modality::Video ||
        modality == model_io::Modality::Document) {
        throw HttpRequestException(
            HttpRequestException::Stage::CreateRequest,
            "chat completions cannot send a " + nlohmann::json(modality).dump()
                + " content part");
    }
    if (modality == model_io::Modality::Image) {
        if (part.type == model_io::ContentType::ExternalRef) return;
        throw HttpRequestException(
            HttpRequestException::Stage::CreateRequest,
            "chat completions sends an image as an external reference, and a "
                + nlohmann::json(part.type).dump()
                + " image carries no media type to describe it with");
    }
    if (part.type == model_io::ContentType::Binary) {
        throw HttpRequestException(
            HttpRequestException::Stage::CreateRequest,
            "chat completions cannot send a binary text part: the base64 "
            "encoding would be delivered as the message");
    }
}

/**
 * Check one part against a TEXT POSITION: assistant replay, replayed reasoning,
 * and tool output are each one string on this wire (assistant_message() and
 * emit_tool_results() concatenate `raw`).
 *
 * Only a text part survives that: anything else — a URL reference to a picture,
 * a document, audio — would be replayed as the characters of its `raw`, which is
 * the silent change of category the label exists to prevent. The rule is per
 * POSITION rather than per Content, because the same part is perfectly legal in a
 * user message; `position` names the field the caller is about to fill.
 */
void require_text_position(const model_io::Content& part, const char* position) {
    if (part.modality == model_io::Modality::Text &&
        part.type != model_io::ContentType::Binary) {
        return;
    }
    throw HttpRequestException(
        HttpRequestException::Stage::CreateRequest,
        std::string("chat completions sends ") + position + " as text, so a "
            + nlohmann::json(part.type).dump() + " "
            + nlohmann::json(part.modality).dump()
            + " part cannot be preserved there");
}

/** Every part of a message that goes out through user_content(). */
void require_input_content(const model_io::MessageItem& item) {
    for (const auto& part : item.content) require_input_part(part);
}

/**
 * Check every part the dialect has to describe, before building anything.
 * Walking up front keeps the failure at construction time and independent of
 * how deep in the conversation the offending part sits.
 *
 * `replay_reasoning` mirrors the dialect opt-in: reasoning that is not sent
 * cannot be unsupported by the provider, so it is not validated. `action_status`
 * is not mapped by this dialect at all and is left alone for the same reason.
 */
void require_supported_conversation(const model_io::AgentInputState& state,
                                    bool replay_reasoning) {
    for (const auto& turn : state.turns) {
        // emit_message() routes an InvokeReturn in a user position through the
        // tool-result path, which is a string; anything else is user content.
        if (turn.user_input.type == model_io::MessageItemType::InvokeReturn) {
            for (const auto& part : turn.user_input.content) {
                require_text_position(part, "a tool result");
            }
        } else {
            require_input_content(turn.user_input);
        }
        for (const auto& step : turn.agent_loop_step) {
            const auto& response = step.model_response;
            for (const auto& part : response.content) {
                require_text_position(part, "an assistant message");
            }
            if (replay_reasoning && response.reasoning) {
                require_text_position(*response.reasoning, "assistant reasoning");
            }
            if (!step.invoke_returns) continue;
            for (const auto& item : *step.invoke_returns) {
                for (const auto& part : item.content) {
                    require_text_position(part, "a tool result");
                }
            }
        }
    }
}

/**
 * One provider part for one Content entry. The media category decides the part
 * kind — never the encoding, and never `extras`: an image is an image whether
 * its URL arrived in `raw` or in `extras.image_url`, while `extras.type` is
 * caller-supplied data and is ignored. The pair was validated above, so the
 * encoding here is one the chosen kind can carry.
 */
json user_content_part(const model_io::Content& content) {
    if (content.modality == model_io::Modality::Image) {
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
        content.front().modality == model_io::Modality::Text) {
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
    // Hard error #3: a part this dialect cannot carry AT THE POSITION it would
    // be emitted at is a construction failure, checked before anything is
    // mapped (see the header contract and the per-position rules above).
    require_supported_conversation(conversation,
                                   _dialect->replay_assistant_reasoning());
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
