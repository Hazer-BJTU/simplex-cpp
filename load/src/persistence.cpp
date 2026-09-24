#include "load/persistence.hpp"

#include "fileio/atomic_write.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <string>
#include <string_view>

namespace load {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

/** Choose a byte prefix without splitting the next UTF-8 code point. */
std::string_view utf8_prefix(std::string_view text, std::size_t maximum) {
    auto length = std::min(text.size(), maximum);
    while (length > 0 && length < text.size()
           && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) {
        --length;
    }
    return text.substr(0, length);
}

/** Fence arbitrary content so embedded headings/backticks cannot break layout. */
void block(std::ostream& output, std::string_view text, std::string_view language) {
    std::size_t longest = 0;
    std::size_t current = 0;
    for (char character : text) {
        current = character == '`' ? current + 1 : 0;
        longest = std::max(longest, current);
    }
    const std::string fence(std::max<std::size_t>(3, longest + 1), '`');
    output << fence << language << '\n' << text;
    if (text.empty() || text.back() != '\n') {
        output << '\n';
    }
    output << fence << "\n\n";
}

/** Bounded JSON pretty-printer; clipping never consumes later Markdown sections. */
class JsonPreview {
public:
    explicit JsonPreview(const ReadableOptions& options) : options_(options) {}

    void write(const Json& value, std::size_t depth = 0) {
        if (full_) {
            return;
        }
        if (value.is_string()) {
            quoted(value.get_ref<const std::string&>());
        } else if (value.is_object() || value.is_array()) {
            if (depth >= options_.max_json_depth) {
                clipped_ = true;
                append(Json("[omitted: depth limit, " + std::to_string(value.size())
                            + " entries]").dump());
                return;
            }
            const bool object = value.is_object();
            append(object ? "{" : "[");
            std::size_t index = 0;
            for (auto it = value.begin(); it != value.end() && !full_; ++it) {
                if (index == options_.max_json_items) {
                    clipped_ = true;
                    append("\n" + std::string((depth + 1) * 2, ' '));
                    append("... [" + std::to_string(value.size() - index) + " entries omitted]");
                    break;
                }
                if (index != 0) {
                    append(",");
                }
                append("\n" + std::string((depth + 1) * 2, ' '));
                if (object) {
                    quoted(it.key());
                    append(": ");
                }
                write(it.value(), depth + 1);
                ++index;
            }
            if (!value.empty()) {
                append("\n" + std::string(depth * 2, ' '));
            }
            append(object ? "}" : "]");
        } else if (value.is_binary()) {
            clipped_ = true;
            append("\"[binary JSON value omitted]\"");
        } else {
            append(value.dump());
        }
    }

    void emit(std::ostream& output) const {
        block(output, text_, "json");
        if (clipped_ || full_) {
            output << "*JSON preview truncated; omitted data remains in the source state.*\n\n";
        }
    }

private:
    /** Bound output even when breadth/depth limits alone permit a large tree. */
    void append(std::string_view text) {
        if (full_) {
            return;
        }
        const auto prefix = utf8_prefix(text, options_.max_json_block_bytes - text_.size());
        text_.append(prefix);
        full_ = prefix.size() != text.size();
    }

    /** Clip raw UTF-8 before JSON escaping, avoiding a full oversized dump. */
    void quoted(std::string_view value) {
        const auto limit = std::min(options_.max_json_string_bytes, options_.max_json_block_bytes);
        const auto prefix = utf8_prefix(value, limit);
        std::string preview(prefix);
        if (prefix.size() != value.size()) {
            clipped_ = true;
            preview += "... [" + std::to_string(value.size() - prefix.size()) + " bytes omitted]";
        }
        append(Json(preview).dump());
    }

    const ReadableOptions& options_;
    std::string text_;
    bool clipped_ = false;
    bool full_ = false;
};

/** Stream a readable view without copying the conversation or its extras. */
class MarkdownWriter {
public:
    MarkdownWriter(std::ostream& output, const ReadableOptions& options)
        : output_(output), options_(options) {}

    void write(const model_io::AgentInputState& state) {
        output_ << "# Agent session\n\n"
                << "Readable export for inspection and memory lookup; not a restorable snapshot.\n\n"
                << "## Session\n\n";
        text("Session ID", state.meta.session_id);
        output_ << "- Status: " << Json(state.meta.status).get<std::string>() << '\n'
                << "- Schema version: " << state.meta.schema_version << '\n'
                << "- Turns: " << state.turns.size() << "\n\n";
        text("Created at", state.meta.created_at);
        text("Updated at", state.meta.updated_at);
        optional_json("Session error", state.meta.error);

        output_ << "## Loop progress\n\n";
        if (state.loop) {
            const auto& loop = *state.loop;
            output_ << "- Status: " << Json(loop.status).get<std::string>() << '\n'
                    << "- Phase: " << Json(loop.phase).get<std::string>() << '\n'
                    << "- Completed exchanges: " << loop.completed_exchanges << '\n'
                    << "- Committed response sequence: " << loop.committed_response_sequence << "\n\n";
            text("Loop error", loop.error);
            for (std::size_t index = 0; index < loop.pending_results.size(); ++index) {
                output_ << "### Pending tool result " << index + 1 << "\n\n";
                result(loop.pending_results[index]);
            }
        } else {
            output_ << "No loop progress recorded.\n\n";
        }

        output_ << "## System prompt\n\n";
        for (const auto& section : state.system_prompt) {
            output_ << "### Prompt section\n\n";
            text("Name", section.name);
            text("Title", section.title);
            text("Stability", Json(section.stability).get<std::string>());
            text("Body", section.text);
        }

        output_ << "## Tools\n\n";
        for (std::size_t index = 0; index < state.tools.size(); ++index) {
            const auto& tool = state.tools[index];
            output_ << "### Tool " << index + 1 << "\n\n";
            text("Name", tool.name);
            text("Description", tool.description);
            json("Argument schema", tool.argument_schema);
            if (tool.remote_type) {
                text("Remote type", *tool.remote_type);
            }
            optional_json("Tool extras", tool.extras);
        }

        output_ << "## Conversation\n\n";
        for (std::size_t turn_index = 0; turn_index < state.turns.size(); ++turn_index) {
            const auto& turn = state.turns[turn_index];
            output_ << "### Turn " << turn_index + 1 << "\n\n";
            text("Retention", Json(turn.retain_priority).get<std::string>());
            output_ << "#### User input\n\n";
            message(turn.user_input);
            for (std::size_t step_index = 0; step_index < turn.agent_loop_step.size(); ++step_index) {
                const auto& step = turn.agent_loop_step[step_index];
                output_ << "#### Step " << step_index + 1 << "\n\n"
                        << "Commit sequence: " << step.commit_sequence << "\n\n";
                text("Retention", Json(step.retain_priority).get<std::string>());
                output_ << "##### Model response\n\n";
                message(step.model_response);
                if (step.invoke_returns) {
                    for (std::size_t index = 0; index < step.invoke_returns->size(); ++index) {
                        output_ << "##### Tool result " << index + 1 << "\n\n";
                        message((*step.invoke_returns)[index]);
                    }
                }
                optional_json("Step extras", step.extras);
            }
            optional_json("Turn extras", turn.extras);
        }
        output_ << "## Session extras\n\n";
        optional_json("Extras", state.extras);
    }

private:
    void text(std::string_view label, std::string_view value) {
        if (!value.empty()) {
            output_ << "**" << label << "**\n\n";
            block(output_, value, "text");
        }
    }

    void json(std::string_view label, const Json& value) {
        output_ << "**" << label << "**\n\n";
        JsonPreview preview(options_);
        preview.write(value);
        preview.emit(output_);
    }

    void optional_json(std::string_view label, const std::optional<Json>& value) {
        if (value) {
            json(label, *value);
        }
    }

    /** Text tool outputs frequently embed JSON; recognize complete containers. */
    void content(std::string_view label, const model_io::Content& value) {
        if (value.type == model_io::ContentType::Binary) {
            text(label, "[binary content omitted; " + std::to_string(value.raw.size()) + " base64 bytes]");
        } else if (value.type == model_io::ContentType::ExternalRef) {
            text(std::string(label) + " (external reference)", value.raw);
        } else {
            const auto start = value.raw.find_first_not_of(" \r\n\t");
            const bool maybe_json = value.type == model_io::ContentType::Text
                && start != std::string::npos
                && (value.raw[start] == '{' || value.raw[start] == '[');
            auto parsed = maybe_json ? Json::parse(value.raw, nullptr, false) : Json();
            if (maybe_json && !parsed.is_discarded()) {
                json(label, parsed);
            } else {
                text(label, value.raw);
            }
        }
        optional_json("Content extras", value.extras);
    }

    void query(const model_io::InvokeQuery& value) {
        text("Tool name", value.name);
        text("Call ID", value.id);
        text("Invocation type", Json(value.type).get<std::string>());
        text("Security", Json(value.security).get<std::string>());
        json("Arguments", value.arguments);
        optional_json("Call extras", value.extras);
    }

    void result(const model_io::InvokeReturn& value) {
        query(value.query);
        content("Output", value.output);
        optional_json("Result extras", value.extras);
    }

    /** Preserve message provenance and separate reasoning, calls, and results. */
    void message(const model_io::MessageItem& value) {
        text("Type", Json(value.type).get<std::string>());
        text("Role", value.role);
        for (const auto& part : value.content) {
            content("Content", part);
        }
        if (value.reasoning) {
            content("Reasoning", *value.reasoning);
        }
        if (value.action_status) {
            content("Action status", *value.action_status);
        }
        if (value.invokes) {
            for (const auto& call : *value.invokes) {
                output_ << "**Tool call**\n\n";
                query(call);
            }
        }
        if (value.invoke_return) {
            output_ << "**Tool result record**\n\n";
            const auto& record = *value.invoke_return;
            query(record.query);
            // The dataclass commonly stores the same output both here and in
            // the first content part. Show it once, but expose differing data.
            const bool already_shown = !value.content.empty()
                && value.content.front().type == record.output.type
                && value.content.front().raw == record.output.raw
                && value.content.front().extras == record.output.extras;
            if (!already_shown) {
                content("Recorded output", record.output);
            }
            optional_json("Result extras", record.extras);
        }
        if (value.cost) {
            json("Tokens (cache_hit is included in prompt)", Json(*value.cost));
        }
        optional_json("Message extras", value.extras);
    }

    std::ostream& output_;
    const ReadableOptions& options_;
};

/** Reject non-JSON values that the JSON text format cannot preserve faithfully. */
void require_json_values(const Json& value) {
    if (value.is_number_float() && !std::isfinite(value.get<double>())) {
        throw std::invalid_argument("snapshot contains a non-finite JSON number");
    }
    if (value.is_binary() || value.is_discarded()) {
        throw std::invalid_argument("snapshot contains a non-JSON value");
    }
    if (value.is_structured()) {
        for (const auto& child : value) {
            require_json_values(child);
        }
    }
}

/** Check record containers that lenient dataclass find() reads could ignore. */
void require_object(const Json& value) {
    if (!value.is_object()) {
        throw std::invalid_argument("snapshot record must be an object");
    }
}

/** Preserve absent/null optional fields while checking present record shapes. */
template<typename Check>
void optional_record(const Json& record, const char* key, Check check) {
    const auto value = record.find(key);
    if (value != record.end() && !value->is_null()) {
        check(*value);
    }
}

/** Iterate records only after ensuring the JSON really is an array. */
template<typename Check>
void record_array(const Json& value, Check check) {
    if (!value.is_array()) {
        throw std::invalid_argument("snapshot record collection must be an array");
    }
    for (const auto& record : value) {
        check(record);
    }
}

/** A returned tool record contains typed query and output records, not scalars. */
void check_result(const Json& value) {
    require_object(value);
    optional_record(value, "query", require_object);
    optional_record(value, "output", require_object);
}

/** Accept legacy single-object content while validating modern message arrays. */
void check_message(const Json& value) {
    require_object(value);
    optional_record(value, "content", [](const Json& content) {
        if (content.is_object()) {
            return;
        }
        record_array(content, require_object);
    });
    for (const auto* key : {"reasoning", "action_status", "cost"}) {
        optional_record(value, key, require_object);
    }
    optional_record(value, "invoke_return", check_result);
    optional_record(value, "invokes", [](const Json& calls) {
        record_array(calls, require_object);
    });
}

/** Validate known record boundaries without interpreting opaque JSON payloads. */
void check_snapshot(const Json& document) {
    require_object(document);
    require_object(document.at("meta"));
    require_object(document.at("system_prompt"));
    optional_record(document.at("system_prompt"), "sections", [](const Json& sections) {
        record_array(sections, require_object);
    });
    record_array(document.at("tools"), require_object);
    record_array(document.at("turns"), [](const Json& turn) {
        require_object(turn);
        optional_record(turn, "user_input", check_message);
        optional_record(turn, "agent_loop_step", [](const Json& steps) {
            record_array(steps, [](const Json& step) {
                require_object(step);
                optional_record(step, "model_response", check_message);
                optional_record(step, "invoke_returns", [](const Json& results) {
                    record_array(results, check_message);
                });
            });
        });
    });
    optional_record(document, "loop", [](const Json& loop) {
        require_object(loop);
        record_array(loop.at("pending_results"), check_result);
    });
}

} // namespace

PersistenceError::~PersistenceError() = default;

void save_state(
    const fs::path& file, const model_io::AgentInputState& state,
    StateFormat format, const ReadableOptions& options) {
    try {
        if (format != StateFormat::Json && format != StateFormat::Readable) {
            throw std::invalid_argument("unknown state format");
        }
        if (format == StateFormat::Readable
            && (options.max_json_string_bytes == 0 || options.max_json_items == 0
                || options.max_json_depth == 0 || options.max_json_depth > 64
                || options.max_json_block_bytes < 64)) {
            throw std::invalid_argument("invalid readable preview limits");
        }
        fileio::atomic_write(file, [&](std::ostream& output) {
            if (format == StateFormat::Json) {
                const Json document = state;
                require_json_values(document);
                output << std::setw(2) << document << '\n';
            } else {
                MarkdownWriter(output, options).write(state);
            }
        });
    } catch (const std::exception& error) {
        throw PersistenceError(file.string() + ": " + error.what());
    }
}

model_io::AgentInputState load_state(const fs::path& file) {
    try {
        std::ifstream input(file, std::ios::binary);
        if (!input) {
            throw std::runtime_error("cannot open JSON snapshot");
        }
        const auto document = Json::parse(input);
        if (input.bad()) {
            throw std::runtime_error("cannot read JSON snapshot");
        }
        check_snapshot(document);
        return document.get<model_io::AgentInputState>();
    } catch (const std::exception& error) {
        throw PersistenceError(file.string() + ": " + error.what());
    }
}

} // namespace load
