// responses/interpreter.cpp — AgentInputState -> POST /responses body.
// Pure data mapping, no I/O; see interpreter.hpp for the layout contract.

#include "endpoint/responses/interpreter.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace endpoint::responses {

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
    if (!emitted && !response.content.raw.empty()) {
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
        } else if (invokes && index < invokes->size() &&
                   !(*invokes)[index].id.empty()) {
            call_id = (*invokes)[index].id;
        }
        if (!call_id.empty()) out["call_id"] = std::move(call_id);
        // content is the contract's canonical payload position; the embedded
        // record's output carries the same bytes and serves as fallback.
        out["output"] = (item.content.raw.empty() && record)
            ? record->output.raw
            : item.content.raw;
        input.push_back(std::move(out));
    }
}

void emit_message_item(json::array_t& input, const model_io::MessageItem& item) {
    json message;
    message["type"] = "message";
    message["role"] = derived_role(item);
    message["content"] = json::array({content_part(item.content, "input_text")});
    input.push_back(std::move(message));
}

} // namespace

ModelRequestInterpreter::HttpRequest ResponsesInterpreter::build_request(
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
    const ResolvedEndpoint where = resolve_endpoint(endpoint);

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

    HttpRequest request{http::verb::post, where.target, 11};
    // RFC 9110 §7.2: the Host authority carries the port when non-default —
    // vhost-routing proxies match on it.
    request.set(http::field::host, where.authority());
    apply_transport_headers(request, endpoint);
    // SSE-specific headers live here, not in the shared helper.
    request.set(http::field::accept, "text/event-stream");
    request.set(http::field::content_type, "application/json");
    request.body() = body.dump();
    request.prepare_payload();
    return request;
}

} // namespace endpoint::responses
