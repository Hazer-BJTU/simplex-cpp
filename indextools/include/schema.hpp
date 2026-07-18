#pragma once

/**
 * @file schema.hpp
 * @brief The authoritative JSON contracts for indextools server responses.
 *
 * There are two top-level wire shapes, both JSON arrays, both defined here as
 * the single source of truth with builders so producers stop hand-assembling
 * (which drifts). The wire format is consumed by out-of-process frontends (a
 * plain-text CLI and a web UI); the C++ side only ever produces this JSON and
 * never renders it.
 *
 *   - DisplayBlock[]  — source-content responses: locate_pattern/identifier/
 *                       entity, check_difference, and the read interface
 *                       (viewer.hpp). See "DisplayBlock" below.
 *   - ProcessReport[] — subprocess status responses: SubProcessManager's
 *                       list_status / collect_finished / collect_running.
 *                       See "ProcessReport" below.
 *
 * The two are deliberately distinct: a display block is `meta` + one body
 * (text xor content), whereas a process report is a status `meta` table plus
 * up to two output streams (stdout/stderr). They share the `meta` convention
 * (parallel field_name/field_content arrays) but nothing else.
 *
 * ## DisplayBlock
 *
 * A top-level result is `DisplayBlock[]`. Each block is:
 *
 *     {
 *       // REQUIRED. A small key/value table: two parallel arrays.
 *       "meta": {
 *         "field_name":    ["File", "Bytes", "Total Bytes", "Truncated"],
 *         "field_content": ["/abs/path", "[100, 4096]", 20480, false]
 *       },
 *
 *       // BODY — exactly one of the following two keys (mutually exclusive):
 *
 *       //  (a) line-indexed body -- used by locate_*, check_difference, and
 *       //      the LINE-level read. Three parallel arrays.
 *       "text": {
 *         "line_content": ["...", "..."],
 *         "line_number":  [40, 41],           // 0-based line indices
 *         "line_type":    ["base", "match"]   // see LineType below
 *       },
 *
 *       //  (b) whole-content body — used by the BYTE-level read. One raw
 *       //      string, NOT split into lines.
 *       "content": "raw slice as a single string ...",
 *
 *       // OPTIONAL. Recursive children (entity trees only): DisplayBlock[].
 *       "sub_entity": [ ...DisplayBlock ]
 *     }
 *
 * A consumer dispatches on which body key is present: `text` → render line by
 * line; `content` → render as one block. `sub_entity`, when present, is
 * rendered recursively after the body.
 *
 * ## LineType
 *
 * Each entry of `text.line_type` is one of:
 *   - "base"   — ordinary/context line.
 *   - "match"  — line directly containing a search hit.
 *   - "add"    — line added (diff).
 *   - "delete" — line removed (diff).
 *
 * ## meta conventions
 *
 * `field_name[i]` labels `field_content[i]`. Values may be strings, numbers or
 * booleans. Ranges are encoded as the string "[start, end]" (end inclusive).
 * Pagination-aware producers SHOULD include a total ("Total Lines"/"Total
 * Bytes") and a boolean "Truncated" so a frontend knows more data remains.
 */

#include "split.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace indextools::schema {

// =============================================================================
// LineType — the closed set of values for text.line_type[]
// =============================================================================

namespace line_type {
inline constexpr std::string_view base   = "base";    ///< Ordinary/context line.
inline constexpr std::string_view match  = "match";   ///< Line contains a hit.
inline constexpr std::string_view add    = "add";     ///< Diff: added line.
inline constexpr std::string_view remove = "delete";  ///< Diff: removed line.
} // namespace line_type

// =============================================================================
// Small value helpers
// =============================================================================

/**
 * @brief Encode an inclusive range as the canonical "[start, end]" string.
 *
 * Used for meta values like a byte range or line range so every producer
 * formats them identically.
 */
inline std::string range_str(size_t start, size_t end_inclusive) {
    return "[" + std::to_string(start) + ", " + std::to_string(end_inclusive) + "]";
}

// =============================================================================
// MetaBuilder — assemble the parallel field_name / field_content arrays
// =============================================================================

/**
 * @brief Accumulates the meta table's two parallel arrays.
 *
 * Each field() call appends one label and its value. build() returns the
 * finished `{"field_name": [...], "field_content": [...]}` object.
 */
class MetaBuilder {
    nlohmann::json _names = nlohmann::json::array();
    nlohmann::json _contents = nlohmann::json::array();

public:
    /// Append one label/value pair. @p value may be string/number/bool/etc.
    template <typename T>
    MetaBuilder& field(std::string_view name, T&& value) {
        _names.push_back(std::string(name));
        _contents.push_back(std::forward<T>(value));
        return *this;
    }

    /// Finished meta object.
    nlohmann::json build() const {
        nlohmann::json meta;
        meta["field_name"] = _names;
        meta["field_content"] = _contents;
        return meta;
    }
};

// =============================================================================
// TextBody — assemble the parallel line_content / line_number / line_type arrays
// =============================================================================

/**
 * @brief Accumulates a line-indexed body (the "text" section).
 *
 * Call line() once per source line to append its content, 0-based number and
 * type; build() returns the finished
 * `{"line_content":[...],"line_number":[...],"line_type":[...]}` object.
 */
class TextBody {
    nlohmann::json _content = nlohmann::json::array();
    nlohmann::json _number  = nlohmann::json::array();
    nlohmann::json _type    = nlohmann::json::array();

public:
    /**
     * @brief Append one line.
     * @param content    Line text (delimiter excluded).
     * @param number     0-based line index.
     * @param type       One of the line_type:: constants.
     */
    TextBody& line(std::string_view content, size_t number,
                   std::string_view type = line_type::base) {
        _content.push_back(std::string(content));
        _number.push_back(number);
        _type.push_back(std::string(type));
        return *this;
    }

    /// Whether no line has been appended yet.
    bool empty() const { return _number.empty(); }

    /// Finished text object.
    nlohmann::json build() const {
        nlohmann::json text;
        text["line_content"] = _content;
        text["line_number"]  = _number;
        text["line_type"]    = _type;
        return text;
    }
};

// =============================================================================
// Block assembly
// =============================================================================

/**
 * @brief Build a display block with a line-indexed ("text") body.
 *
 * @param meta A meta object (typically MetaBuilder::build()).
 * @param text A text object (typically TextBody::build()).
 */
inline nlohmann::json text_block(nlohmann::json meta, nlohmann::json text) {
    nlohmann::json block;
    block["meta"] = std::move(meta);
    block["text"] = std::move(text);
    return block;
}

/**
 * @brief Build a display block with a whole-string ("content") body.
 *
 * @param meta    A meta object (typically MetaBuilder::build()).
 * @param content The raw slice, as a single string (not split into lines).
 */
inline nlohmann::json content_block(nlohmann::json meta, std::string content) {
    nlohmann::json block;
    block["meta"] = std::move(meta);
    block["content"] = std::move(content);
    return block;
}

// =============================================================================
// matched_line_text — text body for locate_pattern / locate_identifier
// =============================================================================

/**
 * @brief Build a "text" body from merged line intervals over a source.
 *
 * Emits one line per index covered by @p expanded_intervals (inclusive ranges,
 * in order). Each line is tagged "match" when @p matched_type[idx] is true and
 * "base" otherwise — the shared shape behind LangAnalyze::locate_pattern() and
 * _build_identifier_lookup_json().
 *
 * @param lines              Source lines; lines[idx] supplies each line's text.
 * @param expanded_intervals Inclusive (start, end) line ranges to emit.
 * @param matched_type       Per-line flag: true → "match", false → "base".
 *                           Must be indexable for every idx in the intervals.
 * @return A finished text object (line_content / line_number / line_type).
 */
inline nlohmann::json matched_line_text(
    const SplitedString& lines,
    const std::vector<std::tuple<size_t, size_t>>& expanded_intervals,
    const std::vector<bool>& matched_type
) {
    TextBody body;
    for (const auto& [line_start, line_end] : expanded_intervals) {
        for (size_t idx = line_start; idx <= line_end; ++idx) {
            body.line(lines[idx], idx,
                      matched_type[idx] ? line_type::match : line_type::base);
        }
    }
    return body.build();
}

// =============================================================================
// ProcessReport — the subprocess status contract (SubProcessManager)
// =============================================================================
//
// A SubProcessManager query returns a JSON `ProcessReport[]`. Each report is:
//
//     {
//       // REQUIRED. Status table, same parallel-array convention as a
//       // DisplayBlock's meta.
//       "meta": {
//         "field_name":    ["ID", "Description", "Status", "Exit Code", "Elapsed (ms)"],
//         "field_content": [0, "task #0", "running", null, 12]
//       },
//
//       // Always present — null when unavailable (list_status) or when
//       // the stream has no output / is not readable.
//       "stdout": "hello\n",
//       "stderr": null
//     }
//
// "Status" is one of "running" | "finished" | "exited" | "unknown" (as computed
// by SubProcessInstance::execution_status()). "Exit Code" is an integer once
// exited, else null.

/**
 * @brief Build one process report (status meta + optional stdout/stderr).
 *
 * Flattens a SubProcessInstance's identity and execution_status() record into
 * the ProcessReport meta table. Call stream() to attach stdout/stderr (the
 * collect_* shape); omit it for the list_status shape (meta only).
 */
class ProcessReport {
    nlohmann::json _meta;
    bool _has_streams = false;
    nlohmann::json _stdout;
    nlohmann::json _stderr;

public:
    /**
     * @brief Construct from identity + an execution_status() record.
     *
     * @param id          Process handle.
     * @param description Human-readable label.
     * @param status      The `{status, exit_code, execution_milliseconds}`
     *                    record returned by execution_status(); its values are
     *                    lifted into the meta table under display labels.
     */
    ProcessReport(uint64_t id, std::string_view description,
                  const nlohmann::json& status) {
        _meta = MetaBuilder()
            .field("ID", id)
            .field("Description", std::string(description))
            .field("Status", status.value("status", nlohmann::json(nullptr)))
            .field("Exit Code", status.value("exit_code", nlohmann::json(nullptr)))
            .field("Elapsed (ms)", status.value("execution_milliseconds", nlohmann::json(nullptr)))
            .build();
    }

    /// Attach the stdout/stderr streams (each a string, or null when absent).
    ProcessReport& stream(nlohmann::json out, nlohmann::json err) {
        _has_streams = true;
        _stdout = std::move(out);
        _stderr = std::move(err);
        return *this;
    }

    /// Finished report object.  stdout/stderr are always present (null when
    /// not attached) so that downstream detection can key on those fields.
    nlohmann::json build() const {
        nlohmann::json report;
        report["meta"] = _meta;
        report["stdout"] = _has_streams ? _stdout : nlohmann::json(nullptr);
        report["stderr"] = _has_streams ? _stderr : nlohmann::json(nullptr);
        return report;
    }
};

// =============================================================================
// ErrorReport — error contract for command validation and execution failures
// =============================================================================
//
// An ErrorReport represents a structured error from any command (search,
// edit, subprocess, etc.). It captures the producing command, the phase
// where the error occurred, a machine-readable code, and a human-readable
// message. Optional structured detail can carry extra context (e.g. a pid,
// file path, or signal number).
//
// JSON shape:
//
//     {
//       "meta": {
//         "field_name":    ["Command", "Phase", "Code"],
//         "field_content": ["search", "validation", "INVALID_ARGUMENT"]
//       },
//       "message": "Parameter 'pattern' cannot be empty.",
//       "detail": null
//     }
//
// "Phase" is one of error_phase::validation or error_phase::execution.
// "Code" is a machine-readable slug drawn from error_code:: (or a custom
// string for domain-specific failures).
// "detail" is always present — null when there is nothing extra, or a
// free-form JSON value with additional context.

// ---------------------------------------------------------------------------
// Error code constants
// ---------------------------------------------------------------------------

namespace error_code {
inline constexpr std::string_view invalid_argument = "INVALID_ARGUMENT";
inline constexpr std::string_view not_found        = "NOT_FOUND";
inline constexpr std::string_view internal_error   = "INTERNAL_ERROR";
inline constexpr std::string_view permission       = "PERMISSION_DENIED";
inline constexpr std::string_view timeout          = "TIMEOUT";
inline constexpr std::string_view subprocess       = "SUBPROCESS_ERROR";
inline constexpr std::string_view io_error         = "IO_ERROR";
} // namespace error_code

// ---------------------------------------------------------------------------
// Phase constants
// ---------------------------------------------------------------------------

namespace error_phase {
inline constexpr std::string_view validation = "validation";
inline constexpr std::string_view execution  = "execution";
} // namespace error_phase

// ---------------------------------------------------------------------------
// ErrorReport builder
// ---------------------------------------------------------------------------

/**
 * @brief Build a structured error report for a command failure.
 *
 * Use the convenience factories validation_error() / execution_error() for
 * the common cases; construct directly only when the phase is dynamic.
 *
 * @code
 *   // Simple validation error
 *   return validation_error("search", error_code::invalid_argument,
 *                           "Pattern must not be empty.").build();
 *
 *   // Execution error with structured detail
 *   return execution_error("subprocess", error_code::timeout,
 *                          "Command timed out after 30 s.")
 *              .detail({{"pid", 1234}, {"signal", "SIGTERM"}})
 *              .build();
 * @endcode
 */
class ErrorReport {
    nlohmann::json _meta;
    std::string _message;
    nlohmann::json _detail;   // null by default

public:
    /**
     * @brief Construct an error report.
     *
     * @param command  Producer label (e.g. "search", "edit", "subprocess").
     * @param phase    One of error_phase::validation / error_phase::execution.
     * @param code     Machine-readable error slug (see error_code::).
     * @param message  Human-readable description.
     */
    ErrorReport(std::string_view command, std::string_view phase,
                std::string_view code, std::string message)
        : _meta(MetaBuilder()
                    .field("Command", std::string(command))
                    .field("Phase",   std::string(phase))
                    .field("Code",    std::string(code))
                    .build())
        , _message(std::move(message))
    {}

    /**
     * @brief Attach structured detail (free-form JSON).
     *
     * Callers typically pass an nlohmann::json object literal:
     * `{{"pid", 1234}, {"path", "/tmp/out"}}`.
     */
    ErrorReport& detail(nlohmann::json d) {
        _detail = std::move(d);
        return *this;
    }

    /// Finished error-report object.
    nlohmann::json build() const {
        nlohmann::json report;
        report["meta"]    = _meta;
        report["message"] = _message;
        report["detail"]  = _detail;
        return report;
    }
};

// ---------------------------------------------------------------------------
// Convenience factories
// ---------------------------------------------------------------------------

/// Build an ErrorReport whose phase is error_phase::validation.
inline ErrorReport validation_error(std::string_view command,
                                     std::string_view code,
                                     std::string message) {
    return ErrorReport(command, error_phase::validation, code, std::move(message));
}

/// Build an ErrorReport whose phase is error_phase::execution.
inline ErrorReport execution_error(std::string_view command,
                                    std::string_view code,
                                    std::string message) {
    return ErrorReport(command, error_phase::execution, code, std::move(message));
}

// =============================================================================
// ExternalRef — sidecar-file response for large or binary content
// =============================================================================
//
// When content is too large to inline (>1 MB) or is a binary object (image,
// serialized blob, etc.), the data is written to a sidecar file and the
// response carries a path + metadata instead of the raw data. The frontend
// reads the file at `path` and renders according to `type`.
//
// JSON shape:
//
//     {
//       "meta": {
//         "field_name":    ["Path", "Bytes", "Type", "Reason"],
//         "field_content": ["/tmp/out.png", 2097152, "image", "size_limit"]
//       },
//       "path": "/tmp/out.png",
//       "message": "Content written to /tmp/out.png"
//     }
//
// "Type" is one of content_type::text / content_type::image /
// content_type::binary.
// "Reason" is one of external_reason::size_limit /
// external_reason::binary_data.
// "message" is auto-generated from path but can be overridden via .message().

// ---------------------------------------------------------------------------
// Content-type constants
// ---------------------------------------------------------------------------

namespace content_type {
inline constexpr std::string_view text   = "text";
inline constexpr std::string_view image  = "image";
inline constexpr std::string_view binary = "binary";
} // namespace content_type

// ---------------------------------------------------------------------------
// Externalisation reason constants
// ---------------------------------------------------------------------------

namespace external_reason {
inline constexpr std::string_view size_limit  = "size_limit";
inline constexpr std::string_view binary_data = "binary_data";
} // namespace external_reason

// ---------------------------------------------------------------------------
// ExternalRef builder
// ---------------------------------------------------------------------------

/**
 * @brief Build a response that points to sidecar-file content.
 *
 * Use this when content cannot (or should not) be inlined in JSON — oversized
 * text, images, binary blobs, etc. The constructor auto-generates a default
 * message; override it with .message() when more context is helpful.
 *
 * @code
 *   // Oversized text result
 *   return ExternalRef("/tmp/result.txt", 2'097'152,
 *                      content_type::text, external_reason::size_limit).build();
 *
 *   // Binary image with a custom message
 *   return ExternalRef("/tmp/plot.png", 524'288,
 *                      content_type::image, external_reason::binary_data)
 *              .message("Chart rendered to /tmp/plot.png")
 *              .build();
 * @endcode
 */
class ExternalRef {
    nlohmann::json _meta;
    std::string _path;
    std::string _message;

public:
    /**
     * @brief Construct an external-content reference.
     *
     * @param path    Absolute path (or file:// URL) to the sidecar file.
     * @param bytes   File size in bytes.
     * @param type    One of content_type::text / image / binary.
     * @param reason  One of external_reason::size_limit / binary_data.
     */
    ExternalRef(std::string_view path, size_t bytes,
                std::string_view type, std::string_view reason)
        : _meta(MetaBuilder()
                    .field("Path",   std::string(path))
                    .field("Bytes",  bytes)
                    .field("Type",   std::string(type))
                    .field("Reason", std::string(reason))
                    .build())
        , _path(path)
        , _message("Content written to " + std::string(path))
    {}

    /**
     * @brief Override the auto-generated message.
     *
     * The default is "Content written to <path>"; use this when the caller
     * needs more detail (e.g. mentioning the original query or truncation).
     */
    ExternalRef& message(std::string msg) {
        _message = std::move(msg);
        return *this;
    }

    /// Finished external-reference object.
    nlohmann::json build() const {
        nlohmann::json report;
        report["meta"]    = _meta;
        report["path"]    = _path;
        report["message"] = _message;
        return report;
    }
};

} // namespace indextools::schema
