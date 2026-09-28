// responses/interpreter.cpp — AgentInputState -> POST /responses body.
// Pure data mapping, no I/O; see interpreter.hpp for the layout contract.

#include "llm/compat/responses/interpreter.hpp"

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace llm::responses {

namespace {

namespace http = boost::beast::http;

using nlohmann::json;

// The wire carries tool arguments as a JSON string; an InvokeQuery.arguments
// that already is one passes through (a lenient backend may have stored the
// raw string), anything else is dumped.
std::string wire_arguments(const json& arguments) {
    if (arguments.is_string()) return arguments.get<std::string>();
    return arguments.dump();
}

// A content part for the input list: the Content's extras (when an object)
// as the base, overlaid with the wire part type and the raw payload — so a
// captured part re-sends its provider fields, and raw stays canonical.
json content_part(const model_io::Content& content, const char* wire_type) {
    json part = (content.extras && content.extras->is_object())
        ? *content.extras
        : json::object();
    part["type"] = wire_type;
    part["text"] = content.raw;
    return part;
}

/**
 * Check one part against the dialect's support matrix and explain the refusal.
 *
 * As in the sibling adapter the matrix is (modality, encoding): the input list
 * has a field for every kind below, but only some representations fit the field
 * the modality maps to. A base64 image has no media type that would let us build
 * an `image_url`, and a text string in `file_data` is not file data. Those
 * combinations are refused here rather than sent as something they are not.
 * This is the module's hard error #3 (see the header contract), and it applies
 * to the INPUT-LIST positions: user content and tool output both go through
 * input_content(). Assistant replay and reasoning are text positions instead —
 * see require_text_position().
 *
 * | modality | encoding     | wire                                     |
 * | -------- | ------------ | ---------------------------------------- |
 * | text     | text         | `input_text`                             |
 * | text     | external_ref | `input_text`; the reference travels as text |
 * | image    | external_ref | `input_image` + `image_url`              |
 * | document | external_ref | `input_file` + `file_url`                |
 * | document | binary       | `input_file` + `file_data`               |
 * | anything else            | refused                                  |
 */
void require_input_part(const model_io::Content& part) {
    const auto modality = part.modality;
    const auto type = part.type;
    const auto refuse = [&](const std::string& reason) {
        throw HttpRequestException(
            HttpRequestException::Stage::CreateRequest,
            "responses cannot send a " + nlohmann::json(type).dump() + " "
                + nlohmann::json(modality).dump() + " content part: " + reason);
    };
    switch (modality) {
        case model_io::Modality::Text:
            if (type == model_io::ContentType::Binary) {
                refuse("the base64 encoding would be delivered as the text");
            }
            return;
        case model_io::Modality::Image:
            if (type != model_io::ContentType::ExternalRef) {
                refuse("input_image takes a URL, and this representation carries "
                       "no media type to describe it with");
            }
            return;
        case model_io::Modality::Document:
            if (type != model_io::ContentType::ExternalRef &&
                type != model_io::ContentType::Binary) {
                refuse("input_file takes a reference or base64 data");
            }
            return;
        case model_io::Modality::Audio:
        case model_io::Modality::Video:
            refuse("the input list has no such part kind");
    }
}

/**
 * Check one part against a TEXT POSITION: a synthesized assistant message goes
 * out as `output_text`, and a synthesized reasoning item as a `summary_text`
 * inside `{type:"reasoning"}`. Both carry `raw` and nothing else.
 *
 * Only a text part survives that. An image or a document passing the input-list
 * matrix and then reaching this path would be replayed as the characters of its
 * URL or its base64, which is exactly the silent change of category the label
 * exists to prevent. `position` names the field the caller is about to fill.
 */
void require_text_position(const model_io::Content& part, const char* position) {
    if (part.modality == model_io::Modality::Text &&
        part.type != model_io::ContentType::Binary) {
        return;
    }
    throw HttpRequestException(
        HttpRequestException::Stage::CreateRequest,
        std::string("responses sends ") + position + " as text, so a "
            + nlohmann::json(part.type).dump() + " "
            + nlohmann::json(part.modality).dump()
            + " part cannot be preserved there");
}

// The wire item captured in an extras record, if it is one of the wanted
// type — the round-trip fast path the stream handler sets up.
const json* captured_item(const std::optional<json>& extras,
                          const char* wire_type) {
    if (!extras || !extras->is_object()) return nullptr;
    const auto type = extras->find("type");
    if (type == extras->end() || !type->is_string()) return nullptr;
    if (type->get<std::string>() != wire_type) return nullptr;
    return &*extras;
}

/** Whether emit_assistant_message() re-emits captured provider items verbatim. */
bool assistant_is_replayed_verbatim(const model_io::MessageItem& response) {
    if (!response.extras || !response.extras->is_object()) return false;
    const auto items = response.extras->find("output_items");
    if (items == response.extras->end() || !items->is_array()) return false;
    for (const auto& item : *items) {
        if (item.is_object() &&
            item.value("type", std::string()) == "message") {
            return true;
        }
    }
    return false;
}

/**
 * Whether emit_reasoning() re-emits captured provider items verbatim. When it
 * does, `reasoning.raw` and its label are not used at all, so the label places
 * no demand on the provider.
 */
bool reasoning_is_replayed_verbatim(const model_io::Content& reasoning) {
    if (!reasoning.extras || !reasoning.extras->is_object()) return false;
    const auto items = reasoning.extras->find("items");
    if (items != reasoning.extras->end() && items->is_array()) {
        for (const auto& item : *items) {
            if (item.is_object()) return true;
        }
    }
    return captured_item(reasoning.extras, "reasoning") != nullptr;
}

/** Whether emit_tool_results() re-emits the captured provider item verbatim. */
bool tool_result_is_replayed_verbatim(const model_io::MessageItem& item) {
    return item.invoke_return
        && captured_item(item.invoke_return->extras, "function_call_output");
}

/**
 * Check every part the dialect has to describe, before building anything.
 * Walking up front keeps the failure at construction time and independent of
 * how deep in the conversation the offending part sits.
 *
 * Each position is checked against the representation it is actually emitted
 * with, and a part that is not emitted at all is not checked: a captured item
 * replayed verbatim does not depend on its Content's label, and `action_status`
 * has no mapping in this dialect, so the provider's capabilities say nothing
 * about it.
 */
void require_supported_conversation(const model_io::AgentInputState& state) {
    for (const auto& turn : state.turns) {
        // A user position and a tool-output position both go through
        // input_content(), so they share the input-list matrix.
        for (const auto& part : turn.user_input.content) require_input_part(part);
        for (const auto& step : turn.agent_loop_step) {
            const auto& response = step.model_response;
            if (!assistant_is_replayed_verbatim(response)) {
                for (const auto& part : response.content) {
                    require_text_position(part, "an assistant message");
                }
            }
            if (response.reasoning &&
                !reasoning_is_replayed_verbatim(*response.reasoning)) {
                require_text_position(*response.reasoning, "assistant reasoning");
            }
            if (!step.invoke_returns) continue;
            for (const auto& item : *step.invoke_returns) {
                if (tool_result_is_replayed_verbatim(item)) continue;
                for (const auto& part : item.content) require_input_part(part);
            }
        }
    }
}

/**
 * Copy the auxiliary provider fields a part kind defines out of `extras`.
 *
 * `extras` reaches the adapter from the conversation, and for user content the
 * caller supplied it, so it is DATA and not an instruction: it may fill in
 * fields of the kind `modality` already chose (a provider-hosted file id, a
 * display filename, an image detail level) and it may never choose the kind
 * itself. That is why the part is built from a whitelist instead of starting as
 * a copy of `extras` — a caller-supplied `"type"` cannot relabel a text part as
 * a file, and no other wire field can be smuggled in beside it.
 */
void inherit(const json& extras, json& part, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        if (auto it = extras.find(key); it != extras.end()) part[key] = *it;
    }
}

/**
 * One provider part for one input Content entry. `modality` decides the part
 * kind, never the encoding and never `extras`: an image is an image whether its
 * URL arrived in `raw` or in `extras.image_url`, and an external reference to a
 * PDF is a file, not a picture. The pair was validated above, so the encoding
 * here is one the chosen kind can carry.
 */
json input_content_part(const model_io::Content& content) {
    const json extras = (content.extras && content.extras->is_object())
        ? *content.extras
        : json::object();

    if (content.modality == model_io::Modality::Image) {
        json part{{"type", "input_image"}};
        inherit(extras, part, {"detail", "file_id"});
        if (!part.contains("file_id")) {
            part["image_url"] = extras.contains("image_url")
                ? extras["image_url"]
                : json(content.raw);
        }
        return part;
    }

    if (content.modality == model_io::Modality::Document) {
        json part{{"type", "input_file"}};
        inherit(extras, part, {"filename", "file_id"});
        if (!part.contains("file_id")) {
            part[content.type == model_io::ContentType::ExternalRef
                ? "file_url" : "file_data"] = content.raw;
        }
        return part;
    }

    // Text: the payload goes out as text whatever the reference says, and
    // input_text has no auxiliary field for extras to fill in.
    return json{{"type", "input_text"}, {"text", content.raw}};
}

// The synthesized assistant content: the output_text part, then — when the
// stream handler parked a refusal in content.extras — a proper refusal part.
// The API models refusal as its own part kind; it must never ride as a member
// of an output_text part (schema-invalid, rejected by strict backends).
json::array_t synthesized_content(const model_io::Content& content) {
    json part = content_part(content, "output_text");
    json refusal;
    if (auto it = part.find("refusal"); it != part.end()) {
        refusal = std::move(*it);
        part.erase("refusal");
    }
    json::array_t parts;
    parts.push_back(std::move(part));
    if (refusal.is_string()) {
        parts.push_back(
            json{{"type", "refusal"}, {"refusal", std::move(refusal)}});
    }
    return parts;
}

json::array_t synthesized_content(
    const std::vector<model_io::Content>& content) {
    json::array_t parts;
    for (const auto& value : content) {
        json::array_t next = synthesized_content(value);
        parts.insert(parts.end(),
                     std::make_move_iterator(next.begin()),
                     std::make_move_iterator(next.end()));
    }
    return parts;
}

json::array_t input_content(const std::vector<model_io::Content>& content) {
    json::array_t parts;
    parts.reserve(content.size());
    for (const auto& value : content) {
        parts.push_back(input_content_part(value));
    }
    return parts;
}

std::string derived_role(const model_io::MessageItem& item) {
    if (!item.role.empty()) return item.role;
    return item.type == model_io::MessageItemType::ModelResponse
        ? std::string("assistant")
        : std::string("user");
}

json synthesized_reasoning(const std::string& raw) {
    return json{
        {"type", "reasoning"},
        {"summary", json::array({
            json{{"type", "summary_text"}, {"text", raw}},
        })},
    };
}

// Reasoning items first inside a model_response. Round-trip first: done
// items captured by the stream handler re-emit verbatim — ids, summaries
// and encrypted_content must survive (the API docs mandate resending them).
void emit_reasoning(json::array_t& input, const model_io::Content& reasoning) {
    if (reasoning.extras && reasoning.extras->is_object()) {
        const json& extras = *reasoning.extras;
        const auto items = extras.find("items");
        if (items != extras.end() && items->is_array()) {
            bool emitted = false;
            for (const auto& item : *items) {
                if (item.is_object()) {
                    input.push_back(item);
                    emitted = true;
                }
            }
            if (emitted) return;
        }
        if (const json* captured = captured_item(reasoning.extras, "reasoning")) {
            input.push_back(*captured);
            return;
        }
    }
    input.push_back(synthesized_reasoning(reasoning.raw));
}

// The assistant message. Round-trip first: message items captured on
// output_item.done re-emit verbatim (annotations / phase / status kept).
// output_items holds every completed item — take only messages here;
// reasoning and calls are emitted from their own fields.
void emit_assistant_message(json::array_t& input,
                            const model_io::MessageItem& response) {
    bool emitted = false;
    if (response.extras && response.extras->is_object()) {
        const auto items = response.extras->find("output_items");
        if (items != response.extras->end() && items->is_array()) {
            for (const auto& item : *items) {
                if (item.is_object() &&
                    item.value("type", std::string()) == "message") {
                    input.push_back(item);
                    emitted = true;
                }
            }
        }
    }
    if (!emitted && std::any_of(response.content.begin(), response.content.end(),
                                [](const auto& part) {
                                    return !part.raw.empty();
                                })) {
        json message;
        message["type"] = "message";
        message["role"] = "assistant";
        message["content"] = synthesized_content(response.content);
        input.push_back(std::move(message));
    }
}

void emit_invokes(json::array_t& input,
                  const std::vector<model_io::InvokeQuery>& invokes) {
    for (const auto& query : invokes) {
        if (const json* captured = captured_item(query.extras, "function_call")) {
            input.push_back(*captured);
            continue;
        }
        input.push_back(json{
            {"type", "function_call"},
            {"call_id", query.id},
            {"name", query.name},
            {"arguments", wire_arguments(query.arguments)},
        });
    }
}

// Tool results, correlated to their calls (the interface contract's three
// steps, most authoritative first): the embedded provenance record's id,
// then positional alignment with the parent response's invokes, then the
// key is omitted (legal — the wire marks call_id optional).
void emit_tool_results(
    json::array_t& input,
    const std::vector<model_io::MessageItem>& results,
    const std::optional<std::vector<model_io::InvokeQuery>>& invokes) {
    for (std::size_t index = 0; index < results.size(); ++index) {
        const model_io::MessageItem& item = results[index];
        const model_io::InvokeReturn* record =
            item.invoke_return ? &*item.invoke_return : nullptr;

        if (record) {
            if (const json* captured =
                    captured_item(record->extras, "function_call_output")) {
                input.push_back(*captured);
                continue;
            }
        }

        json out;
        out["type"] = "function_call_output";
        std::string call_id;
        if (record && !record->query.id.empty()) {
            call_id = record->query.id;
        } else if (invokes && results.size() == invokes->size() &&
                   index < invokes->size() &&
                   !(*invokes)[index].id.empty()) {
            // Positional alignment only when the results map one-to-one onto
            // the invokes — with fewer results than calls it would bind the
            // wrong call_id; omitting the key is the honest failure.
            call_id = (*invokes)[index].id;
        }
        if (!call_id.empty()) out["call_id"] = std::move(call_id);
        // Preserve the Responses API's compact string form for a single text
        // result. Multiple or non-text parts use its heterogeneous output
        // array. The embedded record remains the fallback for an empty list.
        if (item.content.empty()) {
            out["output"] = record ? record->output.raw : std::string();
        } else if (item.content.size() == 1 &&
                   item.content.front().modality == model_io::Modality::Text) {
            out["output"] = (item.content.front().raw.empty() && record)
                ? record->output.raw
                : item.content.front().raw;
        } else {
            out["output"] = input_content(item.content);
        }
        input.push_back(std::move(out));
    }
}

void emit_message_item(json::array_t& input, const model_io::MessageItem& item) {
    json message;
    message["type"] = "message";
    message["role"] = derived_role(item);
    message["content"] = input_content(item.content);
    input.push_back(std::move(message));
}

} // namespace

endpoint::ModelRequestInterpreter::HttpRequest ResponsesInterpreter::build_request(
    const model_io::AgentInputState& conversation,
    const model_io::ModelEndpoint& endpoint,
    const nlohmann::json& generation) {
    // Hard error #1: a non-empty model name is required.
    const auto model = generation.find("model");
    if (model == generation.end() || !model->is_string() ||
        model->get<std::string>().empty()) {
        throw HttpRequestException(
            HttpRequestException::Stage::CreateRequest,
            "generation carries no non-empty \"model\"");
    }
    // Hard error #2 comes with the resolver.
    const endpoint::ResolvedEndpoint where = endpoint::resolve_endpoint(endpoint);
    // Hard error #3: a modality this dialect cannot describe is a construction
    // failure, checked before any part is mapped (see the header contract).
    require_supported_conversation(conversation);

    json body = generation;   // verbatim passthrough; builder keys below win

    if (const auto rendered = conversation.system_prompt.render();
        !rendered.markdown.empty()) {
        body["instructions"] = rendered.markdown;
    }

    json::array_t input;
    for (const model_io::UserLoopStep& turn : conversation.turns) {
        if (turn.user_input.type == model_io::MessageItemType::InvokeReturn) {
            // Lenient: a tool result in a user position still maps through
            // its embedded record — the record says what it is.
            const std::vector<model_io::MessageItem> solo{turn.user_input};
            emit_tool_results(input, solo, std::nullopt);
        } else {
            emit_message_item(input, turn.user_input);
        }

        for (const model_io::AgentLoopStep& step : turn.agent_loop_step) {
            const model_io::MessageItem& response = step.model_response;
            if (response.reasoning) emit_reasoning(input, *response.reasoning);
            emit_assistant_message(input, response);
            if (response.invokes) emit_invokes(input, *response.invokes);
            if (step.invoke_returns) {
                emit_tool_results(input, *step.invoke_returns, response.invokes);
            }
        }
    }
    body["input"] = std::move(input);

    if (!conversation.tools.empty()) {
        json::array_t tools;
        for (const model_io::Invocable& tool : conversation.tools) {
            json definition;
            definition["type"] = "function";
            definition["name"] = tool.name;
            if (!tool.description.empty()) {
                definition["description"] = tool.description;
            }
            definition["parameters"] = tool.argument_schema;
            tools.push_back(std::move(definition));
        }
        body["tools"] = std::move(tools);
    }

    // This layer speaks SSE only — stream is builder-owned, always on (the
    // transport cannot consume a non-streaming JSON reply).
    body["stream"] = true;

    // Default to stateless operation; an explicit generation choice wins.
    bool stored = false;
    if (const auto store = body.find("store");
        store != body.end() && store->is_boolean()) {
        stored = store->get<bool>();
    } else {
        body["store"] = false;
    }

    // Under store=false the reasoning round-trip needs the server to return
    // encrypted reasoning content — ensure that include is requested.
    if (!stored) {
        const auto include = body.find("include");
        if (include == body.end()) {
            body["include"] = json::array({"reasoning.encrypted_content"});
        } else if (include->is_array()) {
            const json wanted = "reasoning.encrypted_content";
            if (std::find(include->begin(), include->end(), wanted) ==
                include->end()) {
                body["include"].push_back(wanted);
            }
        }
    }

    _dialect->transform_request(body);

    HttpRequest request{http::verb::post, where.target, 11};
    // RFC 9110 §7.2: the Host authority carries the port when non-default —
    // vhost-routing proxies match on it.
    request.set(http::field::host, where.authority());
    endpoint::apply_transport_headers(request, endpoint);
    // SSE-specific headers live here, not in the shared helper.
    request.set(http::field::accept, "text/event-stream");
    request.set(http::field::content_type, "application/json");
    request.body() = body.dump();
    request.prepare_payload();
    return request;
}

} // namespace llm::responses
