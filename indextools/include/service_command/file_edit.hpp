#pragma once

/**
 * @file file_edit.hpp
 * @brief Service command for line-range replace/edit operations on files.
 *
 * LineReplaceEditCommand is the service-command wrapper around the
 * line_replace_edit() and check_difference() primitives (editor.hpp). It
 * manages the full lifecycle of a file edit:
 *
 *   1. Validate and normalise parameters (ensure_params).
 *   2. Read the target file into a SplitedString (line-delimited view).
 *   3. Apply the edit via line_replace_edit().
 *   4. Diff the original vs edited content via check_difference().
 *   5. Atomically write the result via a temporary-file + rename strategy.
 *
 * ## Parameters (ensure_params)
 *
 * | Field          | Type    | Required | Default           | Description                              |
 * |----------------|---------|----------|-------------------|------------------------------------------|
 * | file_path      | string  | yes      | —                 | Absolute or relative path to the file.   |
 * | line_start     | int64_t | yes      | —                 | First line of the edit range (0-based).  |
 * | line_end       | int64_t | yes      | —                 | Last line of the edit range (0-based,    |
 * |                |         |          |                   | inclusive). Ignored in insert mode.      |
 * | content        | string  | yes      | —                 | New content to splice in.                |
 * | context_lines  | int64_t | no       | constructor param | Context lines in the diff output.        |
 * | insert_mode    | bool    | no       | false             | If true, insert instead of replace.      |
 *
 * ## Result (on success)
 *
 * Returns the JSON produced by check_difference(): a two-element array of
 * display blocks — [deletions, additions] — each carrying the file path,
 * operated-line counts, and line-numbered content with "delete"/"add"/"base"
 * type tags.
 *
 * ## Error handling
 *
 * - File-not-exists: returns a NOT_FOUND execution error.
 * - Edit-range-out-of-bounds / read/write failure: returns an INTERNAL_ERROR
 *   execution error with the exception message.
 * - Validation failures (missing fields, type mismatches, range inversion):
 *   returned by ensure_params before execute() is called.
 *
 * ## Atomicity
 *
 * The edit is written to a randomly-named temporary file in the same directory
 * as the target, then atomically renamed over the original. On any failure the
 * temporary file is removed if it was created.
 */

#include "service_command.hpp"
#include "editor.hpp"

#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>

namespace indextools {

/**
 * @brief Generate a random 6-character alphanumeric suffix for temporary
 *        file names.
 *
 * The character set is [0-9A-Za-z] (62 characters). Uses a thread_local
 * Mersenne Twister seeded from std::random_device so concurrent calls from
 * different threads produce independent sequences.
 *
 * @return A 6-character string of random alphanumeric characters.
 */
inline std::string random_suffix() {
    static constexpr const char charset[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    // charset has 62 characters + null terminator = 63 bytes.
    // Valid indices are [0, 61]; uniform_int_distribution is inclusive.
    static constexpr size_t kCharsetSize = sizeof(charset) - 1; // exclude null

    thread_local std::mt19937 rng(std::random_device{}());
    thread_local std::uniform_int_distribution<size_t> dist(0, kCharsetSize - 1);

    std::array<char, 6> buf{};
    for (auto& c : buf) {
        c = charset[dist(rng)];
    }
    return std::string(buf.data(), buf.size());
}

/**
 * @brief Service command: replace or insert a line range in a file.
 *
 * Wraps the editor.hpp primitives (line_replace_edit + check_difference) into
 * the ServiceCommand lifecycle. On execution:
 *   - Reads the target file.
 *   - Applies the line-range edit in memory.
 *   - Computes a structured diff.
 *   - Writes the result atomically (temp file → rename).
 *   - Returns the diff as JSON.
 *
 * The number of context lines in the diff output can be configured at
 * construction time and optionally overridden per-request via the
 * `context_lines` parameter.
 */
class LineReplaceEditCommand: public ServiceCommand {
private:
    size_t _context_lines;

public:
    /**
     * @brief Construct with a default context-line count.
     * @param context_lines  Number of surrounding lines to include in the
     *                       diff around each changed line (default: 0).
     */
    explicit LineReplaceEditCommand(size_t context_lines = 0)
        : _context_lines(context_lines) {}

    ~LineReplaceEditCommand() override = default;
    LineReplaceEditCommand(const LineReplaceEditCommand&) = default;
    LineReplaceEditCommand& operator=(const LineReplaceEditCommand&) = default;
    LineReplaceEditCommand(LineReplaceEditCommand&&) noexcept = default;
    LineReplaceEditCommand& operator=(LineReplaceEditCommand&&) noexcept = default;

    std::string_view name() const noexcept override {
        return "file_edit.line_replace_edit";
    }

    nlohmann::json ensure_params(nlohmann::json params,
                                  nlohmann::json& error) const override {
        nlohmann::json ensured_params;

        // ---- required fields ----
        std::string file_path  = expect_field<std::string>(params, "file_path", error);
        int64_t     line_start = expect_field<int64_t>(params, "line_start", error);
        int64_t     line_end   = expect_field<int64_t>(params, "line_end", error);
        std::string content    = expect_field<std::string>(params, "content", error);

        // ---- optional fields ----
        int64_t context_lines = expect_field_or<int64_t>(
            params, "context_lines", static_cast<int64_t>(_context_lines));
        bool insert_mode = expect_field_or<bool>(params, "insert_mode", false);

        // Bail early if any required field was missing or had the wrong type.
        if (!error.empty()) {
            return {};
        }

        // ---- range checks ----
        condition(line_start >= 0,
                  "argument line_start should be non-negative", error);
        condition(line_end >= 0,
                  "argument line_end should be non-negative", error);
        condition(context_lines >= 0,
                  "argument context_lines should be non-negative", error);
        condition(line_start <= line_end,
                  std::format("line_start <= line_end should hold, "
                              "but got line_start = {}; line_end = {}",
                              line_start, line_end),
                  error);

        if (!error.empty()) {
            return {};
        }

        ensured_params = {
            {"file_path",     file_path},
            {"line_start",    line_start},
            {"line_end",      line_end},
            {"content",       content},
            {"context_lines", context_lines},
            {"insert_mode",   insert_mode}
        };

        return ensured_params;
    }

    boost::asio::awaitable<nlohmann::json> execute(
        nlohmann::json ensured_params, nlohmann::json& error) override {

        std::filesystem::path file_path(
            ensured_params["file_path"].get<std::string>());
        size_t line_start    = ensured_params["line_start"];
        size_t line_end      = ensured_params["line_end"];
        std::string content  = ensured_params["content"];
        size_t context_lines = ensured_params["context_lines"];
        bool insert_mode     = ensured_params["insert_mode"];

        // ---- validate target file exists ----
        if (!std::filesystem::exists(file_path)) {
            error.push_back(schema::execution_error(
                name(),
                schema::error_code::not_found,
                std::format("target file path: {} doesn't exist",
                            file_path.string())
            ).build());
            co_return nlohmann::json{};
        }

        nlohmann::json diff;
        std::filesystem::path temporary_path; // tracked for cleanup on failure

        try {
            // ---- 1. read original source ----
            SplitedString splited_source, edited_source;
            splited_source.read(file_path);

            // ---- 2. apply the line-range edit in memory ----
            edited_source.load(line_replace_edit(
                splited_source, line_start, line_end, content, insert_mode));

            // ---- 3. compute structured diff ----
            diff = check_difference(
                splited_source, edited_source, file_path, context_lines);

            // ---- 4. atomically write via temp file + rename ----
            std::string temporary_name =
                file_path.filename().string() + ".tmp" + random_suffix();
            temporary_path = file_path.parent_path() / temporary_name;

            {
                std::ofstream file_out(temporary_path,
                                       std::ios::out | std::ios::trunc);
                if (!file_out.is_open()) {
                    throw std::runtime_error(
                        "failed to create temp file: " + temporary_path.string());
                }
                file_out << edited_source.source();
                if (file_out.fail()) {
                    throw std::runtime_error(
                        "failed to write to temp file: " + temporary_path.string());
                }
                file_out.close();
            }

            // Rename is atomic on most filesystems when source and target
            // are on the same volume (guaranteed here since we write into
            // the target file's own directory).
            std::filesystem::rename(temporary_path, file_path);
        } catch (const std::exception& e) {
            // ---- cleanup: remove temp file if it was created ----
            if (!temporary_path.empty() && std::filesystem::exists(temporary_path)) {
                std::error_code ec;
                std::filesystem::remove(temporary_path, ec);
            }

            error.push_back(schema::execution_error(
                name(),
                schema::error_code::internal_error,
                std::format("exception occurred when editing file: {}", e.what())
            ).build());
        }

        co_return (error.empty() ? diff : nlohmann::json{});
    }
};

/**
 * @brief Service command: find and replace exact substrings in a file.
 *
 * Wraps the editor.hpp primitive str_replace_edit() plus check_difference()
 * into the ServiceCommand lifecycle. On execution:
 *   - Reads the target file.
 *   - Searches for @p original_content as an exact substring.
 *   - Replaces matches with @p inserted_content.
 *   - Computes a structured diff.
 *   - Writes the result atomically (temp file → rename).
 *   - Returns the diff as JSON.
 *
 * ## Parameters (ensure_params)
 *
 * | Field            | Type    | Required | Default           | Description                              |
 * |------------------|---------|----------|-------------------|------------------------------------------|
 * | file_path        | string  | yes      | —                 | Absolute or relative path to the file.   |
 * | original_content | string  | yes      | —                 | Exact substring to search for.           |
 * | inserted_content | string  | yes      | —                 | Replacement text.                        |
 * | replace_all      | bool    | no       | false             | If true, replace every match; otherwise  |
 * |                  |         |          |                   | exactly one match must exist.            |
 * | context_lines    | int64_t | no       | constructor param | Context lines in the diff output.        |
 *
 * ## Result (on success)
 *
 * Returns the JSON produced by check_difference(): a two-element array of
 * display blocks — [deletions, additions] — each carrying the file path,
 * operated-line counts, and line-numbered content with "delete"/"add"/"base"
 * type tags.
 *
 * ## Error handling
 *
 * - File-not-exists: returns a NOT_FOUND execution error.
 * - Pattern not found / ambiguous match (replace_all=false): returned as an
 *   INTERNAL_ERROR execution error (str_replace_edit throws std::runtime_error).
 * - Read/write failure: returns an INTERNAL_ERROR execution error.
 * - Validation failures (missing fields, type mismatches):
 *   returned by ensure_params before execute() is called.
 *
 * ## Atomicity
 *
 * The edit is written to a randomly-named temporary file in the same directory
 * as the target, then atomically renamed over the original. On any failure the
 * temporary file is removed if it was created.
 */
class StrReplaceEditCommand: public ServiceCommand {
private:
    size_t _context_lines;

public:
    /**
     * @brief Construct with a default context-line count.
     * @param context_lines  Number of surrounding lines to include in the
     *                       diff around each changed line (default: 0).
     */
    explicit StrReplaceEditCommand(size_t context_lines = 0)
        : _context_lines(context_lines) {}

    ~StrReplaceEditCommand() override = default;
    StrReplaceEditCommand(const StrReplaceEditCommand&) = default;
    StrReplaceEditCommand& operator=(const StrReplaceEditCommand&) = default;
    StrReplaceEditCommand(StrReplaceEditCommand&&) noexcept = default;
    StrReplaceEditCommand& operator=(StrReplaceEditCommand&&) noexcept = default;

    std::string_view name() const noexcept override {
        return "file_edit.str_replace_edit";
    }

    nlohmann::json ensure_params(nlohmann::json params,
                                  nlohmann::json& error) const override {
        nlohmann::json ensured_params;

        // ---- required fields ----
        std::string file_path       = expect_field<std::string>(params, "file_path", error);
        std::string original_content = expect_field<std::string>(params, "original_content", error);
        std::string inserted_content = expect_field<std::string>(params, "inserted_content", error);

        // ---- optional fields ----
        int64_t context_lines = expect_field_or<int64_t>(
            params, "context_lines", static_cast<int64_t>(_context_lines));
        bool replace_all = expect_field_or<bool>(params, "replace_all", false);

        // Bail early if any required field was missing or had the wrong type.
        if (!error.empty()) {
            return {};
        }

        // ---- range checks ----
        condition(context_lines >= 0,
                  "argument context_lines should be non-negative", error);

        if (!error.empty()) {
            return {};
        }

        ensured_params = {
            {"file_path",        file_path},
            {"original_content", original_content},
            {"inserted_content", inserted_content},
            {"context_lines",    context_lines},
            {"replace_all",      replace_all}
        };

        return ensured_params;
    }

    boost::asio::awaitable<nlohmann::json> execute(
        nlohmann::json ensured_params, nlohmann::json& error) override {

        std::filesystem::path file_path(
            ensured_params["file_path"].get<std::string>());
        std::string original_content = ensured_params["original_content"];
        std::string inserted_content = ensured_params["inserted_content"];
        size_t context_lines = ensured_params["context_lines"];
        bool replace_all     = ensured_params["replace_all"];

        // ---- validate target file exists ----
        if (!std::filesystem::exists(file_path)) {
            error.push_back(schema::execution_error(
                name(),
                schema::error_code::not_found,
                std::format("target file path: {} doesn't exist",
                            file_path.string())
            ).build());
            co_return nlohmann::json{};
        }

        nlohmann::json diff;
        std::filesystem::path temporary_path; // tracked for cleanup on failure

        try {
            // ---- 1. read original source ----
            SplitedString splited_source, edited_source;
            splited_source.read(file_path);

            // ---- 2. apply the string-replace edit in memory ----
            edited_source.load(str_replace_edit(
                splited_source, original_content, inserted_content, replace_all));

            // ---- 3. compute structured diff ----
            diff = check_difference(
                splited_source, edited_source, file_path, context_lines);

            // ---- 4. atomically write via temp file + rename ----
            std::string temporary_name =
                file_path.filename().string() + ".tmp" + random_suffix();
            temporary_path = file_path.parent_path() / temporary_name;

            {
                std::ofstream file_out(temporary_path,
                                       std::ios::out | std::ios::trunc);
                if (!file_out.is_open()) {
                    throw std::runtime_error(
                        "failed to create temp file: " + temporary_path.string());
                }
                file_out << edited_source.source();
                if (file_out.fail()) {
                    throw std::runtime_error(
                        "failed to write to temp file: " + temporary_path.string());
                }
                file_out.close();
            }

            // Rename is atomic on most filesystems when source and target
            // are on the same volume (guaranteed here since we write into
            // the target file's own directory).
            std::filesystem::rename(temporary_path, file_path);
        } catch (const std::exception& e) {
            // ---- cleanup: remove temp file if it was created ----
            if (!temporary_path.empty() && std::filesystem::exists(temporary_path)) {
                std::error_code ec;
                std::filesystem::remove(temporary_path, ec);
            }

            error.push_back(schema::execution_error(
                name(),
                schema::error_code::internal_error,
                std::format("exception occurred when editing file: {}", e.what())
            ).build());
        }

        co_return (error.empty() ? diff : nlohmann::json{});
    }
};

} // namespace indextools
