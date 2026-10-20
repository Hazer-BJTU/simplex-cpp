#include "tools.hpp"
#include "tools/intrinsic/archive_memory/schemas.hpp"
#include "tools/intrinsic/tool_result.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <algorithm>
#include <limits>

namespace tools::intrinsic::archive_memory {
namespace {
namespace asio = boost::asio;
using Json = nlohmann::json;

/** Source bytes consumed and displayed bytes differ after replacement. */
struct DisplaySlice {
    std::string text;
    std::size_t consumed = 0;
    bool replaced = false;
};

/** Copy whole UTF-8 scalars; replace invalid bytes and NUL with visible U+FFFD.
 * Stop BEFORE consuming the source unit that would exceed the display budget.
 * This keeps next_offset lossless even when replacement expands the output. */
DisplaySlice display_slice(std::string_view source, std::size_t budget)
{
    DisplaySlice result;
    while (result.consumed < source.size()) {
        const auto offset = result.consumed;
        const auto first = static_cast<unsigned char>(source[offset]);
        std::size_t width = 1;
        bool valid = first > 0 && first < 0x80;
        if (first >= 0x80) {
            if (first >= 0xc2 && first <= 0xdf) width = 2;
            else if (first >= 0xe0 && first <= 0xef) width = 3;
            else if (first >= 0xf0 && first <= 0xf4) width = 4;
            if (width > 1 && source.size() - offset >= width) {
                valid = true;
                for (std::size_t i = 1; i < width; ++i) {
                    const auto byte = static_cast<unsigned char>(source[offset + i]);
                    if (byte < 0x80 || byte > 0xbf
                        || (i == 1 && first == 0xe0 && byte < 0xa0)
                        || (i == 1 && first == 0xed && byte > 0x9f)
                        || (i == 1 && first == 0xf0 && byte < 0x90)
                        || (i == 1 && first == 0xf4 && byte > 0x8f)) {
                        valid = false;
                        break;
                    }
                }
            }
        }
        const auto emitted = valid ? width : 3;
        if (emitted > budget - result.text.size()) break;
        if (valid) {
            result.text.append(source.substr(offset, width));
            result.consumed += width;
        } else {
            result.text += "\xef\xbf\xbd";
            ++result.consumed;
            result.replaced = true;
        }
    }
    return result;
}

const char* kind_name(persistence::ArchiveTextKind kind)
{
    using enum persistence::ArchiveTextKind;
    switch (kind) {
    case User: return "user";
    case Assistant: return "assistant";
    case ToolBody: return "tool_body";
    case ToolCall: return "tool_call";
    case ToolResult: return "tool_result";
    case PendingToolCall: return "pending_tool_call";
    case PendingToolResult: return "pending_tool_result";
    }
    return "unknown";
}

/** Render complete records only: clipping a locator would make fetch unusable. */
model_io::Content search_output(const persistence::ArchiveSearchResult& result,
                               std::size_t budget)
{
    std::string records;
    std::size_t count = 0;
    for (const auto& hit : result.hits) {
        ToolResult record;
        record.field("archive_id", hit.archive.archive_id)
            .field("source_id", hit.source_id)
            .field("kind", kind_name(hit.kind))
            .field("created_at", hit.archive.created_at)
            .field("offset", hit.start_byte)
            .field("preview_truncated", hit.truncated)
            .block("preview", hit.snippet);
        // 512 bytes reserve covers the fixed header, count and hints below.
        const auto text = "\n---\n\n" + record.text();
        if (text.size() > budget - 512 - records.size()) break;
        records += text;
        ++count;
    }
    const bool more = result.has_more || count < result.hits.size();
    ToolResult header;
    header.field("status", "ok").field("hits", count).field("has_more", more);
    if (more) header.field("hints", "Narrow the query to find additional matches.");
    auto output = header.render();
    output.raw += records;
    return output;
}
} // namespace

QueryTool::QueryTool(const char* declaration,
                     std::shared_ptr<const persistence::ArchiveReader> reader,
                     asio::any_io_executor storage_executor, bool search)
    : DeclaredTool(schema_directory() / declaration),
      reader_(std::move(reader)), storage_executor_(std::move(storage_executor))
{
    initialize_configuration([this, search](const Json& config) {
        for (const auto& [key, value] : config.items()) {
            if (key != "timeout_ms" && key != "max_output_bytes"
                && !(search && key == "snippet_bytes")) {
                configuration_error(key, "unknown archive query configuration field");
            }
        }
        const auto limit = [this, &config](const char* key, std::size_t fallback,
                                          std::size_t minimum, std::size_t maximum) {
            const auto found = config.find(key);
            if (found == config.end()) return fallback;
            if (!found->is_number_integer() || *found < minimum || *found > maximum) {
                configuration_error(key, "must be an integer from "
                    + std::to_string(minimum) + " to " + std::to_string(maximum));
            }
            return found->get<std::size_t>();
        };
        options_.timeout = std::chrono::milliseconds(limit("timeout_ms", 1000, 1, 30000));
        max_output_bytes_ = limit("max_output_bytes", 32768, 2048, 65536);
        if (search) {
            snippet_bytes_ = limit("snippet_bytes", 512, 32, 4096);
            // Keep room for at least one preview, its bounded locators and header.
            if (snippet_bytes_ > max_output_bytes_ - 1536) {
                configuration_error("snippet_bytes", "must leave 1536 bytes for result metadata");
            }
        }
    });
}

void QueryTool::validate_fields(const model_io::InvokeQuery& query,
                                std::initializer_list<std::string_view> allowed)
{
    if (!query.arguments.is_object()) bad_argument("arguments must be an object");
    for (const auto& [key, value] : query.arguments.items()) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            bad_argument("unsupported archive query argument");
        }
    }
}

void QueryTool::validate_text(const std::string& value, const char* name)
{
    if (value.empty() || value.size() > 256 || value.find('\0') != std::string::npos
        || value.find_first_not_of(" \t\n\r\f\v") == std::string::npos
        || display_slice(value, 768).replaced) {
        bad_argument(std::string(name) + " must be nonblank UTF-8, 1..256 bytes, without NUL");
    }
}

void QueryTool::write_attributes(model_io::InvokeQuery& query) const
{
    query.type = model_io::InvokeType::ReadOnly;
    query.security = model_io::InvokeSecurity::Trusted;
}

void QueryTool::query_failed(const persistence::ArchiveQueryError& error)
{
    using enum persistence::ArchiveQueryFailure;
    switch (error.reason()) {
    case MissingStore:
    case NotFound:
        invoke_failed("not_found: archive source is absent or has been pruned; search again");
    case IndexUnavailable:
        invoke_failed("index_unavailable: archive index requires a normal writer upgrade");
    case Cancelled:
        invoke_failed("cancelled: archive query was cancelled");
    case TimedOut:
        invoke_failed("timed_out: archive query exceeded its time budget; narrow the query");
    case InvalidArgument:
        invoke_failed("invalid_argument: invalid archive query or source byte range");
    case StorageError:
        invoke_failed("storage_error: unable to read the session archive store");
    }
    invoke_failed("storage_error: archive query failed");
}

SearchTool::SearchTool(std::shared_ptr<const persistence::ArchiveReader> reader,
                       asio::any_io_executor storage_executor)
    : QueryTool("memory_search.yaml", std::move(reader), std::move(storage_executor), true)
{
}

void SearchTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    validate_fields(query, {"query", "limit"});
    validate_text(require_string(query, "query", "literal text to find"), "query");
    const auto limit = settle_uint(query, "limit", 5);
    if (limit < 1 || limit > 20) bad_argument("limit must be from 1 to 20");
}

asio::awaitable<model_io::Content> SearchTool::invoke(const model_io::InvokeQuery& query)
{
    persistence::ArchiveSearchRequest request;
    request.text = require_string(query, "query", "literal text to find");
    request.limit = optional_uint(query, "limit", 5);
    request.snippet_bytes = snippet_bytes_;
    // Keep the closure alive through co_spawn: capturing coroutine lambdas must
    // not outlive their closure. No task is detached; completion returns to the
    // caller's executor before rendering or updating any loop state.
    auto search = [reader = reader_, request = std::move(request), options = options_]()
        -> asio::awaitable<persistence::ArchiveSearchResult> {
        co_return reader->search(request, options);
    };
    try {
        auto result = co_await asio::co_spawn(storage_executor_, search(), asio::use_awaitable);
        co_return search_output(result, max_output_bytes_);
    } catch (const persistence::ArchiveQueryError& error) {
        if (error.reason() == persistence::ArchiveQueryFailure::MissingStore) {
            co_return search_output({}, max_output_bytes_);
        }
        query_failed(error);
    }
}

FetchTool::FetchTool(std::shared_ptr<const persistence::ArchiveReader> reader,
                     asio::any_io_executor storage_executor)
    : QueryTool("memory_fetch.yaml", std::move(reader), std::move(storage_executor), false)
{
}

void FetchTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    validate_fields(query, {"archive_id", "source_id", "offset", "length"});
    validate_text(require_string(query, "archive_id", "archive from memory_search"), "archive_id");
    validate_text(require_string(query, "source_id", "source from memory_search"), "source_id");
    const auto offset = settle_uint(query, "offset", 0);
    const auto length = settle_uint(query, "length", 8192);
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - 1)) {
        bad_argument("offset exceeds the supported source byte range");
    }
    if (length < 4 || length > 32768) bad_argument("length must be from 4 to 32768 bytes");
}

asio::awaitable<model_io::Content> FetchTool::invoke(const model_io::InvokeQuery& query)
{
    auto read = [reader = reader_, options = options_,
                 archive = require_string(query, "archive_id", "archive from memory_search"),
                 source = require_string(query, "source_id", "source from memory_search"),
                 offset = optional_uint(query, "offset", 0),
                 length = optional_uint(query, "length", 8192)]()
        -> asio::awaitable<persistence::ArchiveExcerpt> {
        co_return reader->read(archive, source, offset, length, options);
    };
    try {
        const auto excerpt = co_await asio::co_spawn(storage_executor_, read(), asio::use_awaitable);
        // Metadata uses only fixed labels, decimal counters and fixed hints; a
        // 1024-byte reserve bounds it independently of user-supplied locators.
        auto display = display_slice(excerpt.text, max_output_bytes_ - 1024);
        const auto next = excerpt.start_byte + display.consumed;
        const bool more = next < excerpt.total_bytes;
        const bool clipped = display.consumed < excerpt.text.size();
        ToolResult output;
        output.field("status", "ok").field("total_bytes", excerpt.total_bytes)
            .field("next_offset", next).field("has_more", more)
            .field("output_truncated", clipped).field("display_replaced", display.replaced);
        if (display.replaced) {
            output.field("hints", "Invalid UTF-8 bytes and NUL replaced for display; offsets address original bytes.");
        }
        output.block("text", std::move(display.text), more);
        co_return output.render();
    } catch (const persistence::ArchiveQueryError& error) {
        query_failed(error);
    }
}

} // namespace tools::intrinsic::archive_memory
