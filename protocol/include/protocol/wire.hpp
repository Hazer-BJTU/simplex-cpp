#pragma once

/**
 * @file protocol/wire.hpp
 * @brief The worker's wire vocabulary, described once and validated by shape.
 *
 * This header is the executable inventory of what crosses the worker boundary.
 * It covers the four families the protocol owns:
 *
 *   1. the persistent connection: payload/signal envelopes, input operations,
 *      history and answer queries, signals, and every emitted event;
 *   2. the shared wire objects those messages embed (content, usage, calls,
 *      results, status/options, history/answer pages, run outcomes, receipts,
 *      diagnostics and display-shortening markers);
 *   3. confirmation request/response envelopes;
 *   4. remote-tool request/response envelopes and their error codes.
 *
 * It is deliberately *not* the internal model. `model_io::AgentInputState` and
 * the projections that read it stay where they are; this header only names the
 * shapes that are actually written to or read from a socket. Fields that are
 * provider-, tool- or plugin-owned are `opaque` and described as such.
 *
 * Ordering note: records are declared in dependency order so a field can name
 * a record defined above it.
 */

#include "protocol/codec.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace protocol {

// ---- Enumerations -----------------------------------------------------------
// Spellings are the exact wire text. `parall_write` is intentionally spelled
// that way: it is the existing classification the hub and worker already share.

enum class EnvelopeType { event, payload, signal, confirmation_request, confirmation_response,
                          tool_request, tool_response };

enum class InputOperation { message, continue_request, compact, history, answer };

enum class SignalOperation { status, options, cancel, shutdown };

enum class ContentType { text, binary, external_ref };

enum class Modality { text, image, audio, video, document };

enum class MessageType { user_input, model_response, invoke_return };

enum class CallScheduling { read_only, parall_write, serial_write };

enum class CallSecurity { default_deny, require_confirm, trusted };

enum class ConfirmationMode { ask, approve, deny };

enum class ConfirmationDecision { approved, denied };

enum class EventName {
    ready, status, options, history, answer, answer_error, history_error,
    input_admitted, input_rejected, run_started, input_committed, model_response,
    tool_calls, tool_results, persisted, compact_finished, export_error, error,
    run_finished,
};

enum class LoopStatus { idle, running, completed, cancelled, exchange_limit, failed,
                        auto_compact_required };

enum class LoopPhase { ready, model, tools, projection, blocked };

enum class RunStatus { completed, cancelled, exchange_limit, failed };

enum class PersistBoundary { before_tools, results_ready, step_finished, run_finished,
                             compact, cancelled, shutdown };

enum class StorageKind { json, sqlite };

enum class RejectionCode { invalid_options, payload_queue_full };

enum class FailureStage { model_request, other };

enum class ToolStatus { succeeded, rejected };

template <>
struct EnumTraits<EnvelopeType> {
    static constexpr std::string_view name = "EnvelopeType";
    static constexpr std::array values{
        EnumValue{EnvelopeType::event, "event"},
        EnumValue{EnvelopeType::payload, "payload"},
        EnumValue{EnvelopeType::signal, "signal"},
        EnumValue{EnvelopeType::confirmation_request, "confirmation_request"},
        EnumValue{EnvelopeType::confirmation_response, "confirmation_response"},
        EnumValue{EnvelopeType::tool_request, "tool_request"},
        EnumValue{EnvelopeType::tool_response, "tool_response"},
    };
};

template <>
struct EnumTraits<InputOperation> {
    static constexpr std::string_view name = "InputOperation";
    // `continue_request` is spelled "continue" on the wire; the enumerator
    // avoids the C++ keyword without changing the protocol.
    static constexpr std::array values{
        EnumValue{InputOperation::message, "message"},
        EnumValue{InputOperation::continue_request, "continue"},
        EnumValue{InputOperation::compact, "compact"},
        EnumValue{InputOperation::history, "history"},
        EnumValue{InputOperation::answer, "answer"},
    };
};

template <>
struct EnumTraits<SignalOperation> {
    static constexpr std::string_view name = "SignalOperation";
    static constexpr std::array values{
        EnumValue{SignalOperation::status, "status"},
        EnumValue{SignalOperation::options, "options"},
        EnumValue{SignalOperation::cancel, "cancel"},
        EnumValue{SignalOperation::shutdown, "shutdown"},
    };
};

template <>
struct EnumTraits<ContentType> {
    static constexpr std::string_view name = "ContentType";
    static constexpr std::array values{
        EnumValue{ContentType::text, "text"},
        EnumValue{ContentType::binary, "binary"},
        EnumValue{ContentType::external_ref, "external_ref"},
    };
};

template <>
struct EnumTraits<Modality> {
    static constexpr std::string_view name = "Modality";
    static constexpr std::array values{
        EnumValue{Modality::text, "text"},
        EnumValue{Modality::image, "image"},
        EnumValue{Modality::audio, "audio"},
        EnumValue{Modality::video, "video"},
        EnumValue{Modality::document, "document"},
    };
};

template <>
struct EnumTraits<MessageType> {
    static constexpr std::string_view name = "MessageType";
    static constexpr std::array values{
        EnumValue{MessageType::user_input, "user_input"},
        EnumValue{MessageType::model_response, "model_response"},
        EnumValue{MessageType::invoke_return, "invoke_return"},
    };
};

template <>
struct EnumTraits<CallScheduling> {
    static constexpr std::string_view name = "CallScheduling";
    static constexpr std::array values{
        EnumValue{CallScheduling::read_only, "read_only"},
        EnumValue{CallScheduling::parall_write, "parall_write"},
        EnumValue{CallScheduling::serial_write, "serial_write"},
    };
};

template <>
struct EnumTraits<CallSecurity> {
    static constexpr std::string_view name = "CallSecurity";
    static constexpr std::array values{
        EnumValue{CallSecurity::default_deny, "default_deny"},
        EnumValue{CallSecurity::require_confirm, "require_confirm"},
        EnumValue{CallSecurity::trusted, "trusted"},
    };
};

template <>
struct EnumTraits<ConfirmationMode> {
    static constexpr std::string_view name = "ConfirmationMode";
    static constexpr std::array values{
        EnumValue{ConfirmationMode::ask, "ask"},
        EnumValue{ConfirmationMode::approve, "approve"},
        EnumValue{ConfirmationMode::deny, "deny"},
    };
};

template <>
struct EnumTraits<ConfirmationDecision> {
    static constexpr std::string_view name = "ConfirmationDecision";
    static constexpr std::array values{
        EnumValue{ConfirmationDecision::approved, "approved"},
        EnumValue{ConfirmationDecision::denied, "denied"},
    };
};

template <>
struct EnumTraits<EventName> {
    static constexpr std::string_view name = "EventName";
    static constexpr std::array values{
        EnumValue{EventName::ready, "ready"},
        EnumValue{EventName::status, "status"},
        EnumValue{EventName::options, "options"},
        EnumValue{EventName::history, "history"},
        EnumValue{EventName::answer, "answer"},
        EnumValue{EventName::answer_error, "answer_error"},
        EnumValue{EventName::history_error, "history_error"},
        EnumValue{EventName::input_admitted, "input_admitted"},
        EnumValue{EventName::input_rejected, "input_rejected"},
        EnumValue{EventName::run_started, "run_started"},
        EnumValue{EventName::input_committed, "input_committed"},
        EnumValue{EventName::model_response, "model_response"},
        EnumValue{EventName::tool_calls, "tool_calls"},
        EnumValue{EventName::tool_results, "tool_results"},
        EnumValue{EventName::persisted, "persisted"},
        EnumValue{EventName::compact_finished, "compact_finished"},
        EnumValue{EventName::export_error, "export_error"},
        EnumValue{EventName::error, "error"},
        EnumValue{EventName::run_finished, "run_finished"},
    };
};

template <>
struct EnumTraits<LoopStatus> {
    static constexpr std::string_view name = "LoopStatus";
    static constexpr std::array values{
        EnumValue{LoopStatus::idle, "idle"},
        EnumValue{LoopStatus::running, "running"},
        EnumValue{LoopStatus::completed, "completed"},
        EnumValue{LoopStatus::cancelled, "cancelled"},
        EnumValue{LoopStatus::exchange_limit, "exchange_limit"},
        EnumValue{LoopStatus::failed, "failed"},
        EnumValue{LoopStatus::auto_compact_required, "auto_compact_required"},
    };
};

template <>
struct EnumTraits<LoopPhase> {
    static constexpr std::string_view name = "LoopPhase";
    static constexpr std::array values{
        EnumValue{LoopPhase::ready, "ready"},
        EnumValue{LoopPhase::model, "model"},
        EnumValue{LoopPhase::tools, "tools"},
        EnumValue{LoopPhase::projection, "projection"},
        EnumValue{LoopPhase::blocked, "blocked"},
    };
};

template <>
struct EnumTraits<RunStatus> {
    static constexpr std::string_view name = "RunStatus";
    static constexpr std::array values{
        EnumValue{RunStatus::completed, "completed"},
        EnumValue{RunStatus::cancelled, "cancelled"},
        EnumValue{RunStatus::exchange_limit, "exchange_limit"},
        EnumValue{RunStatus::failed, "failed"},
    };
};

template <>
struct EnumTraits<PersistBoundary> {
    static constexpr std::string_view name = "PersistBoundary";
    static constexpr std::array values{
        EnumValue{PersistBoundary::before_tools, "before_tools"},
        EnumValue{PersistBoundary::results_ready, "results_ready"},
        EnumValue{PersistBoundary::step_finished, "step_finished"},
        EnumValue{PersistBoundary::run_finished, "run_finished"},
        EnumValue{PersistBoundary::compact, "compact"},
        EnumValue{PersistBoundary::cancelled, "cancelled"},
        EnumValue{PersistBoundary::shutdown, "shutdown"},
    };
};

template <>
struct EnumTraits<StorageKind> {
    static constexpr std::string_view name = "StorageKind";
    static constexpr std::array values{
        EnumValue{StorageKind::json, "json"},
        EnumValue{StorageKind::sqlite, "sqlite"},
    };
};

template <>
struct EnumTraits<RejectionCode> {
    static constexpr std::string_view name = "RejectionCode";
    static constexpr std::array values{
        EnumValue{RejectionCode::invalid_options, "invalid_options"},
        EnumValue{RejectionCode::payload_queue_full, "payload_queue_full"},
    };
};

template <>
struct EnumTraits<FailureStage> {
    static constexpr std::string_view name = "FailureStage";
    static constexpr std::array values{
        EnumValue{FailureStage::model_request, "model_request"},
        EnumValue{FailureStage::other, "other"},
    };
};

template <>
struct EnumTraits<ToolStatus> {
    static constexpr std::string_view name = "ToolStatus";
    static constexpr std::array values{
        EnumValue{ToolStatus::succeeded, "succeeded"},
        EnumValue{ToolStatus::rejected, "rejected"},
    };
};

// ---- Shared wire objects ----------------------------------------------------

/** One ordered content part. Display shortening markers travel with it. */
struct Content {
    ContentType type = ContentType::text;
    Modality modality = Modality::text;
    std::string raw;
    std::optional<Json> extras;
    std::optional<bool> truncated;
    std::optional<std::uint64_t> bytes;
    std::optional<bool> omitted;

    friend constexpr std::string_view wire_name(const Content&) { return "Content"; }
    friend constexpr std::string_view wire_summary(const Content&) {
        return "One ordered content part. Display projections may add truncation markers; "
               "canonical execution bytes never carry them.";
    }
    friend auto wire_fields(const Content&) {
        return std::tuple{
            enumeration<ContentType>("type", &Content::type,
                                     "Encoding of raw: text, base64 binary, or an external reference."),
            enumeration<Modality>("modality", &Content::modality,
                                  "Media category, required and never inferred from type."),
            text("raw", &Content::raw, "UTF-8 text, base64 bytes, or a reference string."),
            opaque("extras", &Content::extras,
                   "Open provider/tool metadata; admitted user metadata is preserved unchanged."),
            boolean("truncated", &Content::truncated,
                    "Display marker: raw is a prefix of the canonical value."),
            integer("bytes", &Content::bytes,
                    "Display marker: original UTF-8 byte length of the shortened value."),
            boolean("omitted", &Content::omitted, "Display marker: the value is not shown here."),
        };
    }
};

/** Provider-reported token usage. Absence of the object means unavailable. */
struct Usage {
    std::uint64_t prompt = 0;
    std::uint64_t generated = 0;
    std::uint64_t cache_hit = 0;

    friend constexpr std::string_view wire_name(const Usage&) { return "Usage"; }
    friend constexpr std::string_view wire_summary(const Usage&) {
        return "Provider-reported token cost; cache_hit counts tokens inside prompt.";
    }
    friend auto wire_fields(const Usage&) {
        return std::tuple{
            integer("prompt", &Usage::prompt, "Prompt tokens reported by the provider."),
            integer("generated", &Usage::generated, "Generated tokens reported by the provider."),
            integer("cache_hit", &Usage::cache_hit,
                    "Prompt tokens served from cache; do not add them to a total."),
        };
    }
};

/** Exact-answer locator: five fields, all required, copied verbatim. */
struct AnswerSource {
    std::string worker_id;
    std::uint64_t turn = 0;
    std::uint64_t step = 0;
    // A decimal string: it is a durable commit identity, not an arithmetic value.
    std::string commit_sequence;
    std::string fingerprint;

    friend constexpr std::string_view wire_name(const AnswerSource&) { return "AnswerSource"; }
    friend constexpr std::string_view wire_summary(const AnswerSource&) {
        return "Locator for exact-answer pagination. Every field must be copied unchanged.";
    }
    friend auto wire_fields(const AnswerSource&) {
        return std::tuple{
            text("worker_id", &AnswerSource::worker_id,
                 "Worker incarnation that committed the response."),
            integer("turn", &AnswerSource::turn, "Zero-based turn index."),
            integer("step", &AnswerSource::step, "Zero-based step index within the turn."),
            text("commit_sequence", &AnswerSource::commit_sequence,
                 "Nonzero commit sequence as a decimal string, never a JSON number."),
            text("fingerprint", &AnswerSource::fingerprint,
                 "SHA-256 over the ordered canonical answer parts of this response.")
                .min_length(1),
        };
    }
    friend UnknownFields wire_unknown_fields(const AnswerSource&) { return UnknownFields::reject; }
};

/** Host-owned provenance of one input or one response. */
struct ExecutionIdentity {
    std::string worker_id;
    std::string request_id;
    std::string run_id;

    friend constexpr std::string_view wire_name(const ExecutionIdentity&) {
        return "ExecutionIdentity";
    }
    friend constexpr std::string_view wire_summary(const ExecutionIdentity&) {
        return "Worker, request and run identity of an input or a committed response.";
    }
    friend auto wire_fields(const ExecutionIdentity&) {
        return std::tuple{
            text("worker_id", &ExecutionIdentity::worker_id, "Worker incarnation."),
            text("request_id", &ExecutionIdentity::request_id, "Admitted payload request."),
            text("run_id", &ExecutionIdentity::run_id, "Run that executed or committed it."),
        };
    }
};

/** One proposed tool call with its scheduling and security classification. */
struct Call {
    std::string id;
    std::string name;
    Json arguments = Json::object();
    CallScheduling type = CallScheduling::read_only;
    CallSecurity security = CallSecurity::trusted;
    std::optional<Json> extras;

    friend constexpr std::string_view wire_name(const Call&) { return "Call"; }
    friend constexpr std::string_view wire_summary(const Call&) {
        return "A proposed tool call. Arguments are tool-owned; the classifications are not.";
    }
    friend auto wire_fields(const Call&) {
        return std::tuple{
            text("id", &Call::id, "Call identity used to correlate its result."),
            text("name", &Call::name, "Tool name."),
            opaque("arguments", &Call::arguments,
                   "Tool-specific arguments as proposed; open JSON, including null. Execution "
                   "validation belongs to the tool, not to a display projection.")
                .nullable_value(),
            enumeration<CallScheduling>("type", &Call::type,
                                        "Scheduling classification; parall_write is the exact spelling."),
            enumeration<CallSecurity>("security", &Call::security,
                                      "Security classification decided before dispatch."),
            opaque("extras", &Call::extras,
                   "Bounded annotations (status, loop_skipped, error preview) copied before arguments."),
        };
    }
};

/** One tool result: the call it answers plus bounded display output. */
struct Result {
    Call query;
    Content output;
    std::optional<Json> extras;

    friend constexpr std::string_view wire_name(const Result&) { return "Result"; }
    friend constexpr std::string_view wire_summary(const Result&) {
        return "A tool result. Recognized extras markers include error, cause_query and loop_skipped.";
    }
    friend auto wire_fields(const Result&) {
        return std::tuple{
            record<Call>("query", &Result::query, "The call this result answers."),
            record<Content>("output", &Result::output, "Human-readable result output."),
            opaque("extras", &Result::extras,
                   "Machine-readable annotations; error.stage and loop_skipped are recognized."),
        };
    }
};

/** One request content part: stricter than a display part. */
struct RequestContent {
    ContentType type = ContentType::text;
    Modality modality = Modality::text;
    std::string raw;
    std::optional<Json> extras;

    friend constexpr std::string_view wire_name(const RequestContent&) { return "RequestContent"; }
    friend constexpr std::string_view wire_summary(const RequestContent&) {
        return "One admitted user part. raw must be nonempty and extras, when present, an object.";
    }
    friend auto wire_fields(const RequestContent&) {
        return std::tuple{
            enumeration<ContentType>("type", &RequestContent::type,
                                     "Encoding of raw; an unknown label is rejected, never inferred."),
            enumeration<Modality>("modality", &RequestContent::modality,
                                  "Media category, required and never inferred from type."),
            text("raw", &RequestContent::raw, "The part body; empty text is rejected.").min_length(1),
            opaque("extras", &RequestContent::extras,
                   "Optional metadata object; an array or scalar is rejected.").object_only(),
        };
    }
};

/**
 * One bounded model-response projection, exactly as `core::project_response()`
 * builds it: the preview fields plus the committed-response cursor.
 */
struct ResponseProjection {
    std::uint64_t index = 0;
    MessageType type = MessageType::model_response;
    std::string role;
    std::vector<Content> content;
    std::uint64_t tool_calls = 0;
    std::uint64_t omitted_parts = 0;
    std::optional<Content> reasoning;
    std::optional<std::string> commit_sequence;
    std::optional<AnswerSource> answer_source;
    std::optional<Usage> cost;
    /** Array of Call, optionally ending in an OmissionMarker. */
    std::optional<Json> invokes;

    friend constexpr std::string_view wire_name(const ResponseProjection&) {
        return "ResponseProjection";
    }
    friend constexpr std::string_view wire_summary(const ResponseProjection&) {
        return "A committed response as displayed: bounded content with its cursor fields. "
               "Exact text is read through answer pages.";
    }
    friend auto wire_fields(const ResponseProjection&) {
        return std::tuple{
            integer("index", &ResponseProjection::index, "Zero-based step index within its turn."),
            enumeration<MessageType>("type", &ResponseProjection::type,
                                     "Conversation record kind; model_response for committed responses."),
            text("role", &ResponseProjection::role, "Conversation role of the response."),
            records<Content>("content", &ResponseProjection::content,
                             "Ordered answer preview; parts beyond the budget are counted."),
            integer("tool_calls", &ResponseProjection::tool_calls,
                    "Number of proposed calls, whether or not they are displayed."),
            integer("omitted_parts", &ResponseProjection::omitted_parts,
                    "Response parts outside this preview."),
            record<Content>("reasoning", &ResponseProjection::reasoning,
                            "Bounded reasoning preview, never part of exact answers."),
            text("commit_sequence", &ResponseProjection::commit_sequence,
                 "Nonzero commit sequence as a decimal string; omitted when uncommitted."),
            record<AnswerSource>("answer_source", &ResponseProjection::answer_source,
                                 "Exact-answer locator for this committed response."),
            record<Usage>("cost", &ResponseProjection::cost,
                          "Provider-reported usage; absence means unavailable, not zero."),
            opaque("invokes", &ResponseProjection::invokes,
                   "Array of Call, optionally ending in an omission marker; see parse_batch(). "
                   "An engaged but empty array is a legitimate projection."),
        };
    }
};

/** One projected tool-result message, as `core::display_results()` builds it. */
struct ToolResultMessage {
    MessageType type = MessageType::invoke_return;
    std::string role;
    std::vector<Content> content;
    std::optional<std::uint64_t> omitted_parts;
    std::optional<Result> invoke_return;
    std::optional<Json> extras;

    friend constexpr std::string_view wire_name(const ToolResultMessage&) {
        return "ToolResultMessage";
    }
    friend constexpr std::string_view wire_summary(const ToolResultMessage&) {
        return "One returned batch entry: the message projection carrying its invoke_return provenance.";
    }
    friend auto wire_fields(const ToolResultMessage&) {
        return std::tuple{
            enumeration<MessageType>("type", &ToolResultMessage::type,
                                     "invoke_return for a tool result."),
            text("role", &ToolResultMessage::role, "Conversation role, normally tool."),
            records<Content>("content", &ToolResultMessage::content,
                             "Bounded tool output; at most four parts are projected."),
            integer("omitted_parts", &ToolResultMessage::omitted_parts,
                    "Output parts outside this projection."),
            record<Result>("invoke_return", &ToolResultMessage::invoke_return,
                           "The call this result answers plus its bounded output."),
            opaque("extras", &ToolResultMessage::extras,
                   "Bounded message annotations; error stage/message are projected separately."),
        };
    }
};

// ---- Options and status -----------------------------------------------------

/** Only `mode` is remotely selectable for confirmation. */
struct ConfirmationOptions {
    std::optional<ConfirmationMode> mode;

    friend constexpr std::string_view wire_name(const ConfirmationOptions&) {
        return "ConfirmationOptions";
    }
    friend constexpr std::string_view wire_summary(const ConfirmationOptions&) {
        return "Per-run confirmation policy; only mode is remotely configurable.";
    }
    friend auto wire_fields(const ConfirmationOptions&) {
        return std::tuple{
            enumeration<ConfirmationMode>("mode", &ConfirmationOptions::mode,
                                          "ask requests approval, approve grants locally, deny refuses locally."),
        };
    }
};

/**
 * Payload options. `model` and `tools` are open category objects on purpose:
 * providers name their own options, and `tools` is reserved and must be empty.
 */
struct Options {
    std::optional<Json> model;
    std::optional<Json> tools;
    std::optional<ConfirmationOptions> confirmation;

    friend constexpr std::string_view wire_name(const Options&) { return "Options"; }
    friend constexpr std::string_view wire_summary(const Options&) {
        return "Optional payload categories applied at the next run boundary; unknown categories are rejected.";
    }
    friend auto wire_fields(const Options&) {
        return std::tuple{
            opaque("model", &Options::model,
                   "Provider-defined keys and values; the adapter validates what it supports.")
                .object_only(),
            opaque("tools", &Options::tools, "Reserved category; it must be an empty object.")
                .object_only().empty_object(),
            record<ConfirmationOptions>("confirmation", &Options::confirmation,
                                        "Confirmation policy for this and later runs."),
        };
    }
    friend UnknownFields wire_unknown_fields(const Options&) { return UnknownFields::reject; }
};

/** One advertised choice list within a category. */
struct OptionChoice {
    std::string name;
    std::vector<std::string> options;

    friend constexpr std::string_view wire_name(const OptionChoice&) { return "OptionChoice"; }
    friend constexpr std::string_view wire_summary(const OptionChoice&) {
        return "An option name with its advertised values, in display order.";
    }
    friend auto wire_fields(const OptionChoice&) {
        return std::tuple{
            text("name", &OptionChoice::name, "Option name the provider or worker accepts."),
            texts("options", &OptionChoice::options, "Advertised values; empty when unknown."),
        };
    }
};

/** One options category: advertised choices plus effective selections. */
struct OptionCategory {
    std::vector<OptionChoice> available;
    Json current = Json::object();

    friend constexpr std::string_view wire_name(const OptionCategory&) { return "OptionCategory"; }
    friend constexpr std::string_view wire_summary(const OptionCategory&) {
        return "Advertised choices and effective current values; an absent key means no selection.";
    }
    friend auto wire_fields(const OptionCategory&) {
        return std::tuple{
            records<OptionChoice>("available", &OptionCategory::available,
                                  "Advertised descriptors in display order."),
            opaque("current", &OptionCategory::current,
                   "Effective values, never a patch to apply; hubs tolerate unknown names."),
        };
    }
};

/** Data of the `options` event. */
struct OptionsData {
    std::optional<OptionCategory> model;
    std::optional<OptionCategory> tools;
    std::optional<OptionCategory> confirmation;

    friend constexpr std::string_view wire_name(const OptionsData&) { return "OptionsData"; }
    friend constexpr std::string_view wire_summary(const OptionsData&) {
        return "Available choices and current selections grouped by category.";
    }
    friend auto wire_fields(const OptionsData&) {
        return std::tuple{
            record<OptionCategory>("model", &OptionsData::model, "Provider options."),
            record<OptionCategory>("tools", &OptionsData::tools,
                                   "Reserved: empty available list and empty current object."),
            record<OptionCategory>("confirmation", &OptionsData::confirmation,
                                   "Confirmation mode choices."),
        };
    }
};

/** Application queue snapshot carried by `status`. */
struct EventQueue {
    std::optional<std::uint64_t> count;
    std::optional<std::uint64_t> bytes;
    std::optional<std::uint64_t> transport_bytes;

    friend constexpr std::string_view wire_name(const EventQueue&) { return "EventQueue"; }
    friend constexpr std::string_view wire_summary(const EventQueue&) {
        return "Queue and transport byte snapshot; a snapshot is not a delivery receipt.";
    }
    friend auto wire_fields(const EventQueue&) {
        return std::tuple{
            integer("count", &EventQueue::count, "Events awaiting transport."),
            integer("bytes", &EventQueue::bytes, "Encoded bytes queued in the application outbox."),
            integer("transport_bytes", &EventQueue::transport_bytes,
                    "Bytes still held by the transport write queue."),
        };
    }
};

struct MemoryRetention {
    std::uint64_t max_archives = 0;

    friend constexpr std::string_view wire_name(const MemoryRetention&) { return "MemoryRetention"; }
    friend constexpr std::string_view wire_summary(const MemoryRetention&) {
        return "Effective archive retention; zero disables cleanup.";
    }
    friend auto wire_fields(const MemoryRetention&) {
        return std::tuple{
            integer("max_archives", &MemoryRetention::max_archives,
                    "Archives kept after compaction; zero disables cleanup."),
        };
    }
};

/** Automatic-compaction counters and effective policy. */
struct AutoCompact {
    std::uint64_t attempts = 0;
    std::uint64_t succeeded = 0;
    std::uint64_t limit = 0;
    std::uint64_t threshold = 0;

    friend constexpr std::string_view wire_name(const AutoCompact&) { return "AutoCompact"; }
    friend constexpr std::string_view wire_summary(const AutoCompact&) {
        return "Attempt/success counters for the current request plus the startup policy; "
               "a zero threshold disables it.";
    }
    friend auto wire_fields(const AutoCompact&) {
        return std::tuple{
            integer("attempts", &AutoCompact::attempts, "Automatic attempts for this logical request."),
            integer("succeeded", &AutoCompact::succeeded, "Successful automatic compactions."),
            integer("limit", &AutoCompact::limit, "Configured attempt limit."),
            integer("threshold", &AutoCompact::threshold,
                    "Token threshold; zero disables the automatic policy."),
        };
    }
};

/** Loop recovery state. Present only when conversation state contains progress. */
struct LoopProgress {
    LoopStatus status = LoopStatus::idle;
    LoopPhase phase = LoopPhase::ready;
    std::uint64_t completed_exchanges = 0;
    std::uint64_t committed_response_sequence = 0;
    std::string error;
    Json pending_results = Json::array();

    friend constexpr std::string_view wire_name(const LoopProgress&) { return "LoopProgress"; }
    friend constexpr std::string_view wire_summary(const LoopProgress&) {
        return "Current or most recent loop outcome and recovery boundary, unrelated to event sequence.";
    }
    friend auto wire_fields(const LoopProgress&) {
        return std::tuple{
            enumeration<LoopStatus>("status", &LoopProgress::status, "Loop outcome."),
            enumeration<LoopPhase>("phase", &LoopProgress::phase, "Recovery boundary."),
            integer("completed_exchanges", &LoopProgress::completed_exchanges,
                    "Model responses committed during that invocation."),
            integer("committed_response_sequence", &LoopProgress::committed_response_sequence,
                    "Persisted count of committed responses; not a replay cursor."),
            text("error", &LoopProgress::error, "Loop-level diagnostic, empty when none."),
            opaque("pending_results", &LoopProgress::pending_results,
                   "Results awaiting projection during recovery; normally an empty array."),
        };
    }
};

/** Data of `ready` and `status`. */
struct StatusData {
    bool active = false;
    bool stopping = false;
    bool storage_failed = false;
    std::uint64_t rejected_payloads = 0;
    std::optional<std::uint64_t> rejected_queries;
    std::optional<std::uint64_t> unreported_rejections;
    std::optional<EventQueue> event_queue;
    std::optional<MemoryRetention> memory_retention;
    std::optional<AutoCompact> auto_compact;
    std::vector<std::string> capabilities;
    std::optional<LoopProgress> loop;

    friend constexpr std::string_view wire_name(const StatusData&) { return "StatusData"; }
    friend constexpr std::string_view wire_summary(const StatusData&) {
        return "State snapshot. Optional fields are omitted by older workers and must be tolerated.";
    }
    friend auto wire_fields(const StatusData&) {
        return std::tuple{
            boolean("active", &StatusData::active, "An admitted invocation is owned, including settlement."),
            boolean("stopping", &StatusData::stopping, "Shutdown is in progress."),
            boolean("storage_failed", &StatusData::storage_failed,
                    "A required persistence write failed; further saves are suppressed."),
            integer("rejected_payloads", &StatusData::rejected_payloads,
                    "Cumulative payload-queue overflow count for this IO client."),
            integer("rejected_queries", &StatusData::rejected_queries,
                    "Cumulative history/answer quota or envelope rejections."),
            integer("unreported_rejections", &StatusData::unreported_rejections,
                    "Rejection notices dropped by a full feedback mailbox."),
            record<EventQueue>("event_queue", &StatusData::event_queue, "Queue snapshot."),
            record<MemoryRetention>("memory_retention", &StatusData::memory_retention,
                                    "Effective archive retention."),
            record<AutoCompact>("auto_compact", &StatusData::auto_compact,
                                "Automatic-compaction counters and policy."),
            texts("capabilities", &StatusData::capabilities,
                  "Features this binary supports; check before querying an older worker."),
            record<LoopProgress>("loop", &StatusData::loop, "Loop recovery state when available."),
        };
    }
};

// ---- Worker requests --------------------------------------------------------

/**
 * One payload request. The wire is a single flat object whose required fields
 * depend on `operation`; `wire_validate` encodes those conditions, so a shape
 * that mixes, say, `history` and `content` fails instead of being half-read.
 */
struct PayloadRequest {
    InputOperation operation = InputOperation::message;
    std::string request_id;
    std::optional<std::vector<RequestContent>> content;
    std::optional<Options> options;
    std::optional<std::uint64_t> start;
    std::optional<std::uint64_t> step;
    std::optional<std::uint64_t> limit;
    std::optional<AnswerSource> source;
    std::optional<std::uint64_t> part;
    std::optional<std::uint64_t> offset;

    friend constexpr std::string_view wire_name(const PayloadRequest&) { return "PayloadRequest"; }
    friend constexpr std::string_view wire_summary(const PayloadRequest&) {
        return "A message, continuation, compaction, history query or answer query; "
               "the operation decides which fields are required.";
    }
    friend auto wire_fields(const PayloadRequest&) {
        return std::tuple{
            enumeration<InputOperation>("operation", &PayloadRequest::operation,
                                        "Selects the request family and its required fields."),
            text("request_id", &PayloadRequest::request_id,
                 "Client-chosen correlation identity; 1 to 128 UTF-8 bytes.")
                .min_length(1).max_length(128),
            records<RequestContent>("content", &PayloadRequest::content,
                                    "Ordered user parts; required for message and refused elsewhere.")
                .non_empty(),
            record<Options>("options", &PayloadRequest::options,
                            "Applied at the next run boundary; refused for history and answer."),
            integer("start", &PayloadRequest::start, "History turn cursor.").default_value("0"),
            integer("step", &PayloadRequest::step, "First step within the history turn.")
                .default_value("0"),
            integer("limit", &PayloadRequest::limit,
                    "History turn count; 1 to 10, applied to the history operation only.")
                .minimum(1).maximum(10).default_value("10"),
            record<AnswerSource>("source", &PayloadRequest::source,
                                 "Exact-answer locator; required for answer only."),
            integer("part", &PayloadRequest::part, "Answer part index, defaulted to 0.")
                .default_value("0"),
            integer("offset", &PayloadRequest::offset, "Byte offset within the part, defaulted to 0.")
                .default_value("0"),
        };
    }
    friend std::span<const Rule> wire_rules(const PayloadRequest&);
    friend std::span<const std::string_view> wire_forbidden_fields(const PayloadRequest&);
    friend bool wire_ignored_field(const PayloadRequest& request, std::string_view name);
    friend bool wire_reject_unknown(const PayloadRequest& request);
};

/** One control signal. Signals have no request ID of their own. */
struct SignalRequest {
    SignalOperation operation = SignalOperation::status;
    std::optional<std::string> run_id;

    friend constexpr std::string_view wire_name(const SignalRequest&) { return "SignalRequest"; }
    friend constexpr std::string_view wire_summary(const SignalRequest&) {
        return "Control signal; additional fields are ignored and no request ID is echoed.";
    }
    friend auto wire_fields(const SignalRequest&) {
        return std::tuple{
            enumeration<SignalOperation>("operation", &SignalRequest::operation,
                                         "status, options, cancel or shutdown."),
            text("run_id", &SignalRequest::run_id,
                 "Required for cancel and rejected otherwise; a stale ID changes nothing."),
        };
    }
    friend std::span<const Rule> wire_rules(const SignalRequest&);
};

// ---- Confirmation -----------------------------------------------------------

/** One confirmation request: a settled call awaiting an operator decision. */
struct ConfirmationRequest {
    std::string worker_id;
    std::string session_id;
    std::string run_id;
    std::string confirmation_id;
    Call call;

    friend constexpr std::string_view wire_name(const ConfirmationRequest&) {
        return "ConfirmationRequest";
    }
    friend constexpr std::string_view wire_summary(const ConfirmationRequest&) {
        return "One confirmation request. Correlate by all four identifiers plus the authenticated "
               "connection; there is no request ID or event sequence.";
    }
    friend auto wire_fields(const ConfirmationRequest&) {
        return std::tuple{
            text("worker_id", &ConfirmationRequest::worker_id, "Worker incarnation asking for a decision."),
            text("session_id", &ConfirmationRequest::session_id, "Session the call belongs to."),
            text("run_id", &ConfirmationRequest::run_id, "Run that proposed the call."),
            text("confirmation_id", &ConfirmationRequest::confirmation_id,
                 "One-shot identity of this confirmation exchange."),
            record<Call>("call", &ConfirmationRequest::call,
                         "The call as settled, not an earlier model proposal."),
        };
    }
};

/** One confirmation response. All four identifiers must match the request. */
struct ConfirmationResponse {
    std::string worker_id;
    std::string session_id;
    std::string run_id;
    std::string confirmation_id;
    ConfirmationDecision decision = ConfirmationDecision::denied;
    std::optional<std::string> reason;

    friend constexpr std::string_view wire_name(const ConfirmationResponse&) {
        return "ConfirmationResponse";
    }
    friend constexpr std::string_view wire_summary(const ConfirmationResponse&) {
        return "One decision for one request. A mismatched identifier denies the call.";
    }
    friend auto wire_fields(const ConfirmationResponse&) {
        return std::tuple{
            text("worker_id", &ConfirmationResponse::worker_id, "Echoed from the request."),
            text("session_id", &ConfirmationResponse::session_id, "Echoed from the request."),
            text("run_id", &ConfirmationResponse::run_id, "Echoed from the request."),
            text("confirmation_id", &ConfirmationResponse::confirmation_id, "Echoed from the request."),
            enumeration<ConfirmationDecision>("decision", &ConfirmationResponse::decision,
                                              "approved or denied; nothing else is accepted."),
            text("reason", &ConfirmationResponse::reason,
                 "Optional operator text; defaults to \"operator decision\"."),
        };
    }
};

// ---- Remote tools -----------------------------------------------------------

/** Terminal rejection detail of a remote-tool exchange. */
struct ToolError {
    std::string code;
    std::string message;

    friend constexpr std::string_view wire_name(const ToolError&) { return "ToolError"; }
    friend constexpr std::string_view wire_summary(const ToolError&) {
        return "Machine-readable rejection code with a diagnostic message.";
    }
    friend auto wire_fields(const ToolError&) {
        return std::tuple{
            text("code", &ToolError::code,
                 "Hub-owned rejection code; unknown codes remain valid diagnostics."),
            text("message", &ToolError::message, "Diagnostic text; not a stable machine code."),
        };
    }
};

/** One remote-tool request. The route is a literal path segment, not a command. */
struct ToolRequest {
    std::string worker_id;
    std::string session_id;
    std::string run_id;
    std::string request_id;
    Json arguments = Json::object();

    friend constexpr std::string_view wire_name(const ToolRequest&) { return "ToolRequest"; }
    friend constexpr std::string_view wire_summary(const ToolRequest&) {
        return "One remote-tool request. The connection path selects a Hub-owned route; "
               "support is decided by the Hub.";
    }
    friend auto wire_fields(const ToolRequest&) {
        return std::tuple{
            text("worker_id", &ToolRequest::worker_id, "Calling worker; must match a live run."),
            text("session_id", &ToolRequest::session_id, "Must match the authenticated route."),
            text("run_id", &ToolRequest::run_id, "Active run of the caller."),
            text("request_id", &ToolRequest::request_id,
                 "Per-attempt correlation; provides neither authorization nor deduplication."),
            opaque("arguments", &ToolRequest::arguments,
                   "Opaque argument object. The Hub handler owns its keys and validation.").object_only(),
        };
    }
};

/** One remote-tool response: either a result object or a terminal rejection. */
struct ToolResponse {
    std::string worker_id;
    std::string session_id;
    std::string run_id;
    std::string request_id;
    std::string route;
    ToolStatus status = ToolStatus::succeeded;
    std::optional<ToolError> error;
    std::optional<Json> result;

    friend constexpr std::string_view wire_name(const ToolResponse&) { return "ToolResponse"; }
    friend constexpr std::string_view wire_summary(const ToolResponse&) {
        return "Echoes the identifiers and route. succeeded carries result, rejected carries error.";
    }
    friend auto wire_fields(const ToolResponse&) {
        return std::tuple{
            text("worker_id", &ToolResponse::worker_id, "Echoed from the request."),
            text("session_id", &ToolResponse::session_id, "Echoed from the request."),
            text("run_id", &ToolResponse::run_id, "Echoed from the request."),
            text("request_id", &ToolResponse::request_id, "Echoed from the request."),
            text("route", &ToolResponse::route, "Echoed route that was executed or rejected."),
            enumeration<ToolStatus>("status", &ToolResponse::status, "succeeded or rejected."),
            record<ToolError>("error", &ToolResponse::error, "Present for rejected only."),
            opaque("result", &ToolResponse::result,
                   "Opaque result object; interpreted by the consuming tool, not the protocol.").object_only(),
        };
    }
    friend void wire_validate(const ToolResponse& response);
};

// ---- History and answer pages ----------------------------------------------

/** One model step inside a history page. */
struct HistoryStep {
    std::uint64_t index = 0;
    std::vector<Content> content;
    std::uint64_t tool_calls = 0;
    std::uint64_t omitted_parts = 0;
    std::optional<Content> reasoning;
    std::optional<std::string> commit_sequence;
    std::optional<AnswerSource> answer_source;
    std::optional<ExecutionIdentity> execution;

    friend constexpr std::string_view wire_name(const HistoryStep&) { return "HistoryStep"; }
    friend constexpr std::string_view wire_summary(const HistoryStep&) {
        return "One bounded response preview. Exact text is read through answer pages.";
    }
    friend auto wire_fields(const HistoryStep&) {
        return std::tuple{
            integer("index", &HistoryStep::index, "Zero-based step index within its turn."),
            records<Content>("content", &HistoryStep::content, "Ordered answer preview."),
            integer("tool_calls", &HistoryStep::tool_calls,
                    "Number of proposed calls; arguments and results are not returned."),
            integer("omitted_parts", &HistoryStep::omitted_parts,
                    "Response parts outside this preview."),
            record<Content>("reasoning", &HistoryStep::reasoning, "Bounded reasoning preview."),
            text("commit_sequence", &HistoryStep::commit_sequence,
                 "Nonzero commit sequence as a decimal string; omitted for legacy steps."),
            record<AnswerSource>("answer_source", &HistoryStep::answer_source,
                                 "Exact-answer locator when a fresh source is available."),
            record<ExecutionIdentity>("execution", &HistoryStep::execution,
                                      "Execution that produced this response."),
        };
    }
};

/** One turn inside a history page. */
struct HistoryTurn {
    std::uint64_t index = 0;
    std::vector<Content> user;
    std::vector<HistoryStep> steps;
    std::uint64_t omitted_user_parts = 0;
    std::uint64_t omitted_steps = 0;
    std::optional<ExecutionIdentity> source;
    std::optional<std::string> internal_input;

    friend constexpr std::string_view wire_name(const HistoryTurn&) { return "HistoryTurn"; }
    friend constexpr std::string_view wire_summary(const HistoryTurn&) {
        return "One canonical turn with a repeated user projection; steps are admitted whole.";
    }
    friend auto wire_fields(const HistoryTurn&) {
        return std::tuple{
            integer("index", &HistoryTurn::index, "Zero-based canonical turn index."),
            records<Content>("user", &HistoryTurn::user, "Bounded ordered user preview."),
            records<HistoryStep>("steps", &HistoryTurn::steps, "Model steps on this page."),
            integer("omitted_user_parts", &HistoryTurn::omitted_user_parts,
                    "User parts outside the display projection."),
            integer("omitted_steps", &HistoryTurn::omitted_steps,
                    "Model steps after this page that remain to be fetched."),
            record<ExecutionIdentity>("source", &HistoryTurn::source,
                                      "Original input provenance, preserved across continue."),
            text("internal_input", &HistoryTurn::internal_input,
                 "auto_compact_continue for the hidden host-owned continuation input."),
        };
    }
};

/** Data of the `history` event. */
struct HistoryPage {
    std::string request_id;
    std::uint64_t revision = 0;
    std::uint64_t start = 0;
    std::uint64_t step = 0;
    std::uint64_t next = 0;
    std::uint64_t next_step = 0;
    std::uint64_t total = 0;
    std::vector<HistoryTurn> turns;

    friend constexpr std::string_view wire_name(const HistoryPage&) { return "HistoryPage"; }
    friend constexpr std::string_view wire_summary(const HistoryPage&) {
        return "One history page. All pages of a load must agree on worker, revision and total.";
    }
    friend auto wire_fields(const HistoryPage&) {
        return std::tuple{
            text("request_id", &HistoryPage::request_id, "Query this page answers."),
            integer("revision", &HistoryPage::revision,
                    "Display-state revision within one worker; not persisted."),
            integer("start", &HistoryPage::start, "Requested turn cursor."),
            integer("step", &HistoryPage::step, "Requested step cursor."),
            integer("next", &HistoryPage::next, "Next turn cursor; equals total at the end."),
            integer("next_step", &HistoryPage::next_step,
                    "Next step cursor; zero when the turn is finished."),
            integer("total", &HistoryPage::total, "Total turns in this revision."),
            records<HistoryTurn>("turns", &HistoryPage::turns, "Turns on this page."),
        };
    }
};

/** Data of the `answer` event: one exact content segment. */
struct AnswerPage {
    std::string request_id;
    AnswerSource source;
    std::uint64_t part = 0;
    std::uint64_t offset = 0;
    std::string raw;
    ContentType type = ContentType::text;
    Modality modality = Modality::text;
    std::uint64_t bytes = 0;
    std::uint64_t next_offset = 0;
    std::uint64_t next_part = 0;
    std::uint64_t total_parts = 0;
    bool done = false;

    friend constexpr std::string_view wire_name(const AnswerPage&) { return "AnswerPage"; }
    friend constexpr std::string_view wire_summary(const AnswerPage&) {
        return "Exact answer bytes with the cursor to continue; binary parts are unavailable.";
    }
    friend auto wire_fields(const AnswerPage&) {
        return std::tuple{
            text("request_id", &AnswerPage::request_id, "Query this page answers."),
            record<AnswerSource>("source", &AnswerPage::source, "Echoed source."),
            integer("part", &AnswerPage::part, "Echoed part index."),
            integer("offset", &AnswerPage::offset, "Echoed byte offset."),
            text("raw", &AnswerPage::raw, "Original bytes, verbatim."),
            enumeration<ContentType>("type", &AnswerPage::type, "Encoding of raw."),
            enumeration<Modality>("modality", &AnswerPage::modality, "Media category of raw."),
            integer("bytes", &AnswerPage::bytes, "Original byte length of this part."),
            integer("next_offset", &AnswerPage::next_offset, "Offset to continue within this part."),
            integer("next_part", &AnswerPage::next_part, "Next part index; reset offset when it advances."),
            integer("total_parts", &AnswerPage::total_parts, "Parts in the committed response."),
            boolean("done", &AnswerPage::done, "True for the last part."),
        };
    }
};

/** Data of events that carry no payload. */
struct EmptyData {
    friend constexpr std::string_view wire_name(const EmptyData&) { return "EmptyData"; }
    friend constexpr std::string_view wire_summary(const EmptyData&) {
        return "An empty object. Older workers may omit fields; nothing is required.";
    }
    friend auto wire_fields(const EmptyData&) { return std::tuple{}; }
};

/** Data of `history_error` and `answer_error`. */
struct QueryErrorData {
    Json request_id = nullptr;
    std::string message;

    friend constexpr std::string_view wire_name(const QueryErrorData&) { return "QueryErrorData"; }
    friend constexpr std::string_view wire_summary(const QueryErrorData&) {
        return "Query failure. request_id echoes whatever the query carried, including null.";
    }
    friend auto wire_fields(const QueryErrorData&) {
        return std::tuple{
            opaque("request_id", &QueryErrorData::request_id,
                   "Echoed query identity; any JSON value, including null.", true),
            text("message", &QueryErrorData::message, "Diagnostic text."),
        };
    }
};

/** Data of `input_admitted`. */
struct InputAdmittedData {
    std::optional<InputOperation> operation;

    friend constexpr std::string_view wire_name(const InputAdmittedData&) { return "InputAdmittedData"; }
    friend constexpr std::string_view wire_summary(const InputAdmittedData&) {
        return "Admitted operation retained for transcript replay; older workers emitted an empty object.";
    }
    friend auto wire_fields(const InputAdmittedData&) {
        return std::tuple{
            enumeration<InputOperation>("operation", &InputAdmittedData::operation,
                                        "message, continue or compact."),
        };
    }
};

/** Data of `input_rejected`. */
struct InputRejectedData {
    Json request_id = nullptr;
    std::string message;
    std::optional<InputOperation> operation;
    std::optional<RejectionCode> code;

    friend constexpr std::string_view wire_name(const InputRejectedData&) { return "InputRejectedData"; }
    friend constexpr std::string_view wire_summary(const InputRejectedData&) {
        return "An input that failed validation or queue admission; no run was started for it.";
    }
    friend auto wire_fields(const InputRejectedData&) {
        return std::tuple{
            opaque("request_id", &InputRejectedData::request_id,
                   "Rejected request identity; any JSON value, including null.", true),
            text("message", &InputRejectedData::message, "Diagnostic text."),
            enumeration<InputOperation>("operation", &InputRejectedData::operation,
                                        "Recognized operation, kept for transcript replay."),
            enumeration<RejectionCode>("code", &InputRejectedData::code,
                                       "invalid_options or payload_queue_full."),
        };
    }
};

/** Archive reference published by a successful compaction. */
struct ArchiveRef {
    std::string session_id;
    std::string archive_id;
    StorageKind storage_kind = StorageKind::json;
    bool restorable = true;

    friend constexpr std::string_view wire_name(const ArchiveRef&) { return "ArchiveRef"; }
    friend constexpr std::string_view wire_summary(const ArchiveRef&) {
        return "Storage evidence of a compaction archive; never an answer-paging source.";
    }
    friend auto wire_fields(const ArchiveRef&) {
        return std::tuple{
            text("session_id", &ArchiveRef::session_id, "Session the archive belongs to."),
            text("archive_id", &ArchiveRef::archive_id, "Archive identity."),
            enumeration<StorageKind>("storage_kind", &ArchiveRef::storage_kind, "Backend that holds it."),
            boolean("restorable", &ArchiveRef::restorable, "Whether the archive can be restored."),
        };
    }
};

/** Cleanup counters attached to a compaction. */
struct ArchiveCleanup {
    std::uint64_t removed_archives = 0;
    std::uint64_t removed_bytes = 0;

    friend constexpr std::string_view wire_name(const ArchiveCleanup&) { return "ArchiveCleanup"; }
    friend constexpr std::string_view wire_summary(const ArchiveCleanup&) {
        return "Archives removed after a successful compaction.";
    }
    friend auto wire_fields(const ArchiveCleanup&) {
        return std::tuple{
            integer("removed_archives", &ArchiveCleanup::removed_archives, "Archives deleted."),
            integer("removed_bytes", &ArchiveCleanup::removed_bytes, "Bytes reclaimed."),
        };
    }
};

/** Oversized or unavailable summary, reported instead of a truncated prefix. */
struct OmissionSummary {
    bool display_omitted = true;
    std::uint64_t bytes = 0;
    std::string reason;

    friend constexpr std::string_view wire_name(const OmissionSummary&) {
        return "OmissionSummary";
    }
    friend constexpr std::string_view wire_summary(const OmissionSummary&) {
        return "An explicit omission instead of an apparently complete summary prefix.";
    }
    friend auto wire_fields(const OmissionSummary&) {
        return std::tuple{
            boolean("display_omitted", &OmissionSummary::display_omitted,
                    "Always true when present."),
            integer("bytes", &OmissionSummary::bytes, "Original UTF-8 byte length of the summary."),
            text("reason", &OmissionSummary::reason, "Why the summary is not shown here."),
        };
    }
};

/** Data of `compact_finished`. */
struct CompactFinishedData {
    /** The summary string, or an OmissionSummary object for an oversized one. */
    Json summary = std::string();
    std::optional<std::string> memory_file;
    std::optional<ArchiveRef> archive;
    std::uint64_t removed_turns = 0;
    std::uint64_t revision = 0;
    bool durable = true;
    std::optional<std::string> origin;
    std::optional<std::uint64_t> cycle;
    std::optional<std::string> call_id;
    std::optional<ArchiveCleanup> archive_cleanup;
    std::optional<std::string> archive_cleanup_error;

    friend constexpr std::string_view wire_name(const CompactFinishedData&) {
        return "CompactFinishedData";
    }
    friend constexpr std::string_view wire_summary(const CompactFinishedData&) {
        return "Compacted state was durably published; old history pages must be invalidated.";
    }
    friend auto wire_fields(const CompactFinishedData&) {
        return std::tuple{
            opaque("summary", &CompactFinishedData::summary,
                   "The published summary: a string, or an omission object when it exceeds "
                   "the 32 KiB budget."),
            text("memory_file", &CompactFinishedData::memory_file, "Memory file written, when any."),
            record<ArchiveRef>("archive", &CompactFinishedData::archive, "Archive the summary came from."),
            integer("removed_turns", &CompactFinishedData::removed_turns, "Turns replaced by the summary."),
            integer("revision", &CompactFinishedData::revision, "Display revision after compaction."),
            boolean("durable", &CompactFinishedData::durable, "Whether the replacement was published."),
            text("origin", &CompactFinishedData::origin,
                 "automatic for an automatic compaction; absent for a manual one."),
            integer("cycle", &CompactFinishedData::cycle,
                    "Automatic attempt number that produced this summary."),
            text("call_id", &CompactFinishedData::call_id,
                 "Call that triggered the automatic compaction."),
            record<ArchiveCleanup>("archive_cleanup", &CompactFinishedData::archive_cleanup,
                                   "Cleanup counters on success."),
            text("archive_cleanup_error", &CompactFinishedData::archive_cleanup_error,
                 "Cleanup diagnostic on failure; the run outcome is unaffected."),
        };
    }
};

/** Data of `persisted`. */
struct PersistedData {
    PersistBoundary boundary = PersistBoundary::before_tools;
    StorageKind backend = StorageKind::json;
    std::optional<std::uint64_t> revision;
    std::string operation_id;

    friend constexpr std::string_view wire_name(const PersistedData&) { return "PersistedData"; }
    friend constexpr std::string_view wire_summary(const PersistedData&) {
        return "A required snapshot write completed at the named boundary; notifications may be coalesced.";
    }
    friend auto wire_fields(const PersistedData&) {
        return std::tuple{
            enumeration<PersistBoundary>("boundary", &PersistedData::boundary, "Checkpoint that completed."),
            enumeration<StorageKind>("backend", &PersistedData::backend, "Backend that wrote it."),
            integer("revision", &PersistedData::revision,
                    "Backend revision; the key is always present and null when the backend "
                    "does not expose one.", true)
                .required(),
            text("operation_id", &PersistedData::operation_id, "Write operation identity."),
        };
    }
};

/** Data of `export_error`. */
struct ExportErrorData {
    std::string operation;
    std::string message;

    friend constexpr std::string_view wire_name(const ExportErrorData&) { return "ExportErrorData"; }
    friend constexpr std::string_view wire_summary(const ExportErrorData&) {
        return "Optional archive-cleanup diagnostic; it never changes the run outcome.";
    }
    friend auto wire_fields(const ExportErrorData&) {
        return std::tuple{
            text("operation", &ExportErrorData::operation, "Operation that failed."),
            text("message", &ExportErrorData::message, "Diagnostic text."),
        };
    }
};

/** Failure classification of a failed run. */
struct FailureData {
    FailureStage stage = FailureStage::other;
    std::string operation;
    bool can_continue = false;

    friend constexpr std::string_view wire_name(const FailureData&) { return "FailureData"; }
    friend constexpr std::string_view wire_summary(const FailureData&) {
        return "Where a run failed and whether an explicit continue may be admitted.";
    }
    friend auto wire_fields(const FailureData&) {
        return std::tuple{
            enumeration<FailureStage>("stage", &FailureData::stage,
                                      "model_request when the request raised before committing."),
            text("operation", &FailureData::operation, "task, compact or auto_compact."),
            boolean("can_continue", &FailureData::can_continue,
                    "True when a turn or valid host memory exists and the phase is ready."),
        };
    }
};

/** Data of `run_finished`. */
struct RunOutcomeData {
    RunStatus status = RunStatus::completed;
    std::string error;
    std::uint64_t exchanges = 0;
    std::optional<std::uint64_t> task_exchanges;
    std::optional<std::uint64_t> compact_exchanges;
    std::optional<AutoCompact> auto_compact;
    bool durable = false;
    std::optional<FailureData> failure;
    std::optional<std::uint64_t> omitted_display_events;
    std::optional<std::uint64_t> omitted_query_events;
    std::optional<std::uint64_t> omitted_feedback_events;
    std::optional<std::uint64_t> coalesced_metadata_events;

    friend constexpr std::string_view wire_name(const RunOutcomeData&) { return "RunOutcomeData"; }
    friend constexpr std::string_view wire_summary(const RunOutcomeData&) {
        return "Settled invocation outcome. Omission counters appear only when nonzero and are "
               "not lifetime totals or delivery acknowledgements.";
    }
    friend auto wire_fields(const RunOutcomeData&) {
        return std::tuple{
            enumeration<RunStatus>("status", &RunOutcomeData::status, "Final logical-request outcome."),
            text("error", &RunOutcomeData::error, "Technical diagnostic, empty when none."),
            integer("exchanges", &RunOutcomeData::exchanges,
                    "Committed task and compact responses in this logical request."),
            integer("task_exchanges", &RunOutcomeData::task_exchanges, "Committed task responses."),
            integer("compact_exchanges", &RunOutcomeData::compact_exchanges,
                    "Committed summary responses."),
            record<AutoCompact>("auto_compact", &RunOutcomeData::auto_compact,
                                "Automatic-compaction counters and policy."),
            boolean("durable", &RunOutcomeData::durable,
                    "Whether a final or cancellation snapshot was recorded."),
            record<FailureData>("failure", &RunOutcomeData::failure, "Present for failed only."),
            integer("omitted_display_events", &RunOutcomeData::omitted_display_events,
                    "Display events omitted since the previous settlement."),
            integer("omitted_query_events", &RunOutcomeData::omitted_query_events,
                    "Query responses omitted since the previous settlement."),
            integer("omitted_feedback_events", &RunOutcomeData::omitted_feedback_events,
                    "Rejection feedback omitted since the previous settlement."),
            integer("coalesced_metadata_events", &RunOutcomeData::coalesced_metadata_events,
                    "Replayable metadata events coalesced since the previous settlement."),
        };
    }
};

/** Data of `error`. */
struct ErrorData {
    std::string message;
    std::optional<bool> durable;
    std::optional<Json> primary_outcome;
    std::optional<std::string> storage_error;

    friend constexpr std::string_view wire_name(const ErrorData&) { return "ErrorData"; }
    friend constexpr std::string_view wire_summary(const ErrorData&) {
        return "Control-validation or storage diagnostic, not a universal fatal notification. "
               "A terminal storage failure always carries primary_outcome.";
    }
    friend auto wire_fields(const ErrorData&) {
        return std::tuple{
            text("message", &ErrorData::message, "Diagnostic text."),
            boolean("durable", &ErrorData::durable, "Present only as false for storage failures."),
            opaque("primary_outcome", &ErrorData::primary_outcome,
                   "Status, error and operation of the outcome this failure superseded."),
            text("storage_error", &ErrorData::storage_error,
                 "Describes an uncertain archive mutation."),
        };
    }
};

/** One trailing omission marker inside a tool batch array. */
struct OmissionMarker {
    bool display_omitted = true;
    std::uint64_t omitted_items = 0;

    friend constexpr std::string_view wire_name(const OmissionMarker&) { return "OmissionMarker"; }
    friend constexpr std::string_view wire_summary(const OmissionMarker&) {
        return "Counts a batch suffix that was not projected; it is not a call or a result.";
    }
    friend auto wire_fields(const OmissionMarker&) {
        return std::tuple{
            boolean("display_omitted", &OmissionMarker::display_omitted, "Always true when present."),
            integer("omitted_items", &OmissionMarker::omitted_items, "Entries left out of this batch."),
        };
    }
};

// ---- Envelopes --------------------------------------------------------------

/** The persistent-connection event envelope. */
struct EventEnvelope {
    EnvelopeType type = EnvelopeType::event;
    EventName event = EventName::ready;
    std::string session_id;
    std::string worker_id;
    std::string request_id;
    std::string run_id;
    std::uint64_t sequence = 0;
    Json data = Json::object();

    friend constexpr std::string_view wire_name(const EventEnvelope&) { return "EventEnvelope"; }
    friend constexpr std::string_view wire_summary(const EventEnvelope&) {
        return "An emitted event. data carries the shape registered for this event name.";
    }
    friend auto wire_fields(const EventEnvelope&) {
        return std::tuple{
            enumeration<EnvelopeType>("type", &EventEnvelope::type, "Always \"event\".").constant("event"),
            enumeration<EventName>("event", &EventEnvelope::event, "Event name."),
            text("session_id", &EventEnvelope::session_id, "Worker session."),
            text("worker_id", &EventEnvelope::worker_id, "Worker incarnation that emitted it."),
            text("request_id", &EventEnvelope::request_id,
                 "Current admitted request, empty when none."),
            text("run_id", &EventEnvelope::run_id, "Current run, empty when none."),
            integer("sequence", &EventEnvelope::sequence,
                    "Monotonic event sequence allocated at admission; not a replay cursor."),
            opaque("data", &EventEnvelope::data, "Event data; its shape is registered per event name."),
        };
    }
    friend void wire_validate(const EventEnvelope& envelope);
};

struct PayloadEnvelope {
    EnvelopeType type = EnvelopeType::payload;
    PayloadRequest data;

    friend constexpr std::string_view wire_name(const PayloadEnvelope&) { return "PayloadEnvelope"; }
    friend constexpr std::string_view wire_summary(const PayloadEnvelope&) {
        return "Hub-to-worker payload envelope.";
    }
    friend auto wire_fields(const PayloadEnvelope&) {
        return std::tuple{
            enumeration<EnvelopeType>("type", &PayloadEnvelope::type, "Always \"payload\".").constant("payload"),
            record<PayloadRequest>("data", &PayloadEnvelope::data, "The request."),
        };
    }
    friend void wire_validate(const PayloadEnvelope& envelope);
};

struct SignalEnvelope {
    EnvelopeType type = EnvelopeType::signal;
    SignalRequest data;

    friend constexpr std::string_view wire_name(const SignalEnvelope&) { return "SignalEnvelope"; }
    friend constexpr std::string_view wire_summary(const SignalEnvelope&) {
        return "Hub-to-worker signal envelope; it uses a separate queue from payloads.";
    }
    friend auto wire_fields(const SignalEnvelope&) {
        return std::tuple{
            enumeration<EnvelopeType>("type", &SignalEnvelope::type, "Always \"signal\".").constant("signal"),
            record<SignalRequest>("data", &SignalEnvelope::data, "The signal."),
        };
    }
    friend void wire_validate(const SignalEnvelope& envelope);
};

struct ConfirmationRequestEnvelope {
    EnvelopeType type = EnvelopeType::confirmation_request;
    ConfirmationRequest data;

    friend constexpr std::string_view wire_name(const ConfirmationRequestEnvelope&) {
        return "ConfirmationRequestEnvelope";
    }
    friend constexpr std::string_view wire_summary(const ConfirmationRequestEnvelope&) {
        return "Worker-to-hub confirmation request on its own connection.";
    }
    friend auto wire_fields(const ConfirmationRequestEnvelope&) {
        return std::tuple{
            enumeration<EnvelopeType>("type", &ConfirmationRequestEnvelope::type,
                                      "Always \"confirmation_request\".").constant("confirmation_request"),
            record<ConfirmationRequest>("data", &ConfirmationRequestEnvelope::data, "The request."),
        };
    }
};

struct ConfirmationResponseEnvelope {
    EnvelopeType type = EnvelopeType::confirmation_response;
    ConfirmationResponse data;

    friend constexpr std::string_view wire_name(const ConfirmationResponseEnvelope&) {
        return "ConfirmationResponseEnvelope";
    }
    friend constexpr std::string_view wire_summary(const ConfirmationResponseEnvelope&) {
        return "Hub-to-worker decision, sent once on the same confirmation connection.";
    }
    friend auto wire_fields(const ConfirmationResponseEnvelope&) {
        return std::tuple{
            enumeration<EnvelopeType>("type", &ConfirmationResponseEnvelope::type,
                                      "Always \"confirmation_response\".").constant("confirmation_response"),
            record<ConfirmationResponse>("data", &ConfirmationResponseEnvelope::data, "The decision."),
        };
    }
};

struct ToolRequestEnvelope {
    EnvelopeType type = EnvelopeType::tool_request;
    ToolRequest data;

    friend constexpr std::string_view wire_name(const ToolRequestEnvelope&) {
        return "ToolRequestEnvelope";
    }
    friend constexpr std::string_view wire_summary(const ToolRequestEnvelope&) {
        return "One-shot remote-tool request; the route is the connection path.";
    }
    friend auto wire_fields(const ToolRequestEnvelope&) {
        return std::tuple{
            enumeration<EnvelopeType>("type", &ToolRequestEnvelope::type,
                                      "Always \"tool_request\".").constant("tool_request"),
            record<ToolRequest>("data", &ToolRequestEnvelope::data, "The request."),
        };
    }
};

struct ToolResponseEnvelope {
    EnvelopeType type = EnvelopeType::tool_response;
    ToolResponse data;

    friend constexpr std::string_view wire_name(const ToolResponseEnvelope&) {
        return "ToolResponseEnvelope";
    }
    friend constexpr std::string_view wire_summary(const ToolResponseEnvelope&) {
        return "Terminal remote-tool answer; the connection closes after it.";
    }
    friend auto wire_fields(const ToolResponseEnvelope&) {
        return std::tuple{
            enumeration<EnvelopeType>("type", &ToolResponseEnvelope::type,
                                      "Always \"tool_response\".").constant("tool_response"),
            record<ToolResponse>("data", &ToolResponseEnvelope::data, "The response."),
        };
    }
};

/** A projected batch: entries plus the optional trailing omission count. */
template <class R>
struct Batch {
    std::vector<R> items;
    std::optional<OmissionMarker> omitted;
};

/**
 * Parse a batch array. The trailing omission marker is the only non-entry
 * element the producers emit, and only in the final position.
 */
template <class R>
Batch<R> parse_batch(const Json& value, const std::string& path = "data") {
    if (!value.is_array()) {
        throw ValidationError(path, "expected an array");
    }
    Batch<R> batch;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto& entry = value.at(index);
        const std::string here = path + "[" + std::to_string(index) + "]";
        if (entry.is_object() && entry.contains("display_omitted")) {
            if (index + 1 != value.size()) {
                throw ValidationError(here, "an omission marker must be the final element");
            }
            batch.omitted = protocol::from_json<OmissionMarker>(entry, here);
            continue;
        }
        batch.items.push_back(protocol::from_json<R>(entry, here));
    }
    return batch;
}

// ---- Cross-record rules -----------------------------------------------------

/**
 * Validate one event's `data` against the record registered for its name.
 * Batches (`tool_calls`, `tool_results`) are arrays whose last element may be an
 * omission marker; every other element must be the registered record.
 */
void validate_event_data(const EventEnvelope& envelope);

/** One published message family. */
struct MessageSpec {
    const char* family = "";
    const RecordDescriptor* record = nullptr;
};

std::span<const MessageSpec> message_registry();

/** The registry view shared by the manifest and by focused tests. */
struct EventDataSpec {
    EventName event;
    const RecordDescriptor* record;
    /** True when data is an array of `record`, optionally ending in a marker. */
    bool array = false;
};

std::span<const EventDataSpec> event_data_registry();

} // namespace protocol
