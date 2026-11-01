#include "tools/intrinsic/hub_remote_call/subagents.hpp"
#include "tools/intrinsic/tool_result.hpp"
#include "tools/intrinsic/file_work.hpp"
#include "fileio/new_file.hpp"
#include "fileio/atomic_write.hpp"
#include <boost/asio/this_coro.hpp>
#include <algorithm>
#include <chrono>

namespace tools::intrinsic {
namespace {
using Json = nlohmann::json;
void check(bool condition) {
    if (!condition) throw std::invalid_argument("invalid subagent receive response");
}
std::string clipped(std::string text, std::size_t maximum) {
    if (text.size() <= maximum) return text;
    auto end = maximum;
    while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    text.resize(end);
    return text;
}
void status(ToolResult& out, const Json& value) {
    check(value.is_object() && value.at("subagent_id").is_string());
    for (const auto* key : {"subagent_id", "lifecycle", "health", "connected", "pending_approvals", "active"}) {
        if (value.contains(key)) {
            const auto& field = value.at(key);
            check(field.is_primitive());
            out.field(key, field.is_string() ? Json(clipped(field.get<std::string>(), 256)) : field);
        }
    }
    if (value.contains("error") && value.at("error").is_string())
        out.field("error", clipped(value.at("error").get<std::string>(), 1024));
}
}

void SubagentReceiveTool::ensure_arguments(model_io::InvokeQuery& query) const {
    try {
        const auto& args = query.arguments;
        check(args.is_object() && args.dump().size() <= 64 * 1024);
        for (const auto& [key, value] : args.items()) check(key == "subagent_id" || key == "output_path");
        if (args.contains("subagent_id")) {
            const auto id = args.at("subagent_id").get<std::string>();
            check(!id.empty() && id.size() <= 128 && id.find_first_not_of(
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos);
        }
        if (args.contains("output_path")) {
            const auto path = args.at("output_path").get<std::string>();
            check(args.contains("subagent_id") && !path.empty() && path.find('\0') == std::string::npos);
        }
    } catch (const std::exception&) {
        bad_argument("receive accepts subagent_id and optional nonempty output_path; export requires a target");
    }
}
void SubagentReceiveTool::write_attributes(model_io::InvokeQuery& query) const {
    const bool exporting = query.arguments.contains("output_path");
    query.type = exporting ? model_io::InvokeType::SerialWrite : model_io::InvokeType::ReadOnly;
    query.security = exporting ? model_io::InvokeSecurity::RequireConfirm : model_io::InvokeSecurity::Trusted;
}

boost::asio::awaitable<model_io::Content> SubagentReceiveTool::invoke(const model_io::InvokeQuery& query) {
    co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
    try {
        const auto identity = identity_();
        const bool exporting = query.arguments.contains("output_path");
        auto remote = query;
        remote.arguments.erase("output_path");
        if (exporting) remote.arguments["export"] = Json::object();
        else remote.arguments["view"] = "preview";
        const auto exchange = [&](model_io::InvokeQuery call) -> boost::asio::awaitable<Json> {
            auto reply = co_await request(std::move(call), route_, identity.worker_id, identity.session_id, identity.run_id);
            if (reply.at("status") != "succeeded") {
                auto code = reply.at("error").value("code", "rejected");
                if (code.empty() || code.size() > 80 || !std::all_of(code.begin(), code.end(),
                        [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; })) code = "rejected";
                // Only the bounded error code is exposed, never endpoint or arbitrary reply data.
                invoke_failed("subagent receive rejected: " + clipped(code, 80));
            }
            co_return reply.at("result");
        };
        auto value = co_await exchange(remote);
        ToolResult out;
        if (!query.arguments.contains("subagent_id")) {
            const auto& children = value.at("subagents");
            check(children.is_array());
            out.field("subagents", children.size());
            std::size_t shown = 0;
            for (const auto& child : children) {
                if (shown++ >= 32) break;
                out.separate(); status(out, child);
            }
            if (children.size() > 32) out.field("omitted_children", children.size() - 32);
            co_return out.render();
        }
        check(value.at("subagent_id") == query.arguments.at("subagent_id"));
        if (!exporting) {
            status(out, value);
            const auto& preview = value.at("preview");
            const auto text = preview.at("response").get<std::string>();
            for (const auto* key : {"truncated", "stale", "incomplete", "history_truncated"}) {
                check(preview.at(key).is_boolean());
                out.field(key, key == std::string_view("truncated")
                    ? Json(preview.at(key).get<bool>() || text.size() > 16 * 1024) : preview.at(key));
            }
            if (preview.contains("request_id")) {
                check(preview.at("request_id").is_string());
                out.field("response_request_id", clipped(preview.at("request_id").get<std::string>(), 128));
            }
            if (value.contains("latest_request") && !value.at("latest_request").is_null()) {
                const auto& latest = value.at("latest_request");
                for (const auto* key : {"request_id", "operation", "state", "status"}) {
                    check(latest.at(key).is_string() || latest.at(key).is_null());
                    out.field(key, latest.at(key).is_string() ? Json(clipped(latest.at(key).get<std::string>(), 256)) : Json());
                }
            }
            const bool truncated = preview.at("truncated").get<bool>() || text.size() > 16 * 1024;
            if (truncated) out.field("hint", "Use output_path to export the saved conversation.");
            out.block("response", clipped(text, 16 * 1024), truncated);
            co_return out.render();
        }
        // Every IO task owns the writer while running; cancellation cannot leave
        // a partial final file. Keep blocking filesystem operations off the loop.
        auto work = std::make_shared<FileWork>();
        const auto path = std::filesystem::absolute(query.arguments.at("output_path").get<std::string>());
        auto file = co_await work->run([path](std::stop_token) { return std::make_shared<fileio::NewFile>(path); });
        std::uint64_t offset = 0, total = 0;
        std::string token;
        Json metadata;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        std::exception_ptr failure;
        try {
            for (;;) {
                check(std::chrono::steady_clock::now() < deadline);
                check(value.at("subagent_id") == query.arguments.at("subagent_id"));
                const auto& page = value.at("export");
                check(page.at("source") == "persisted_snapshot" && page.at("offset") == offset);
                check(page.at("total").is_number_unsigned() && page.at("next").is_number_unsigned());
                check(page.at("done").is_boolean() && page.at("blocks").is_number_unsigned() && page.at("omitted").is_boolean());
                check(page.at("latest_block").is_null() || (page.at("latest_block").is_string()
                    && page.at("latest_block").get_ref<const std::string&>().size() <= 256));
                const auto text = page.at("text").get<std::string>();
                const auto next = page.at("next").get<std::uint64_t>();
                if (offset == 0) {
                    total = page.at("total").get<std::uint64_t>();
                    token = page.at("token").get<std::string>();
                    check(!token.empty() && token.size() <= 128 && total > 0 && total <= 64 * 1024 * 1024);
                    metadata = {{"blocks", page.at("blocks")}, {"omitted", page.at("omitted")}, {"latest_block", page.at("latest_block")}};
                }
                check(page.at("token") == token && page.at("total") == total && !text.empty()
                    && text.size() <= 32 * 1024 && next == offset + text.size() && next <= total
                    && page.at("done").get<bool>() == (next == total));
                for (const auto& [key, field] : metadata.items()) check(page.at(key) == field);
                co_await work->run([file, text](std::stop_token) { file->append(text); });
                offset = next;
                if (next == total) break;
                remote.arguments["export"] = {{"token", token}, {"offset", offset}};
                value = co_await exchange(remote);
            }
            co_await work->run([file](std::stop_token) { file->publish(); });
        } catch (...) { failure = std::current_exception(); }
        co_await work->run([owned = std::move(file)](std::stop_token) mutable { owned.reset(); });
        if (failure) std::rethrow_exception(failure);
        out.field("subagent_id", query.arguments.at("subagent_id"))
            .field("output_path", path.string()).field("bytes", total).field("blocks", metadata.at("blocks"))
            .field("non_text_omitted", metadata.at("omitted"))
            .field("hint", "Latest saved snapshot; running work may not yet be saved. Search simplex:block: markers.");
        if (!metadata.at("latest_block").is_null()) out.field("latest_block", metadata.at("latest_block"));
        co_return out.render();
    } catch (const fileio::AtomicWriteError& error) {
        invoke_failed(error.published() ? "export file was published, but durability or cleanup is uncertain; inspect it before retrying"
            : "could not write export; parent directory must exist and output_path must be unused");
    } catch (const tools::InvokeException&) { throw; }
    catch (const std::exception&) {
        invoke_failed("subagent receive failed; no complete export was published; retry with a new file path after checking child status");
    }
}
}
