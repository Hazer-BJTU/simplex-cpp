#define BOOST_TEST_MODULE CoreArchiveMemory
#include <boost/test/unit_test.hpp>
#include "core/application.hpp"
#include "core/protocol.hpp"
#include "persistence/store.hpp"
#include <boost/asio/use_future.hpp>
#include <boost/beast.hpp>
#include <algorithm>

namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
using Json = nlohmann::json;

/** Inspect the actual model input after backend selection and prompt refresh. */
struct Model : llm::LLMModel {
    bool expected;
    bool called = false;
    bool retrieve;
    unsigned phase = 0;
    Model(asio::any_io_executor executor, bool enabled, bool use_tools)
        : LLMModel(executor, {}), expected(enabled), retrieve(use_tools) {}

    llm::LLMModelType model_type() const noexcept override { return llm::LLMModelType::Conversation; }

    asio::awaitable<model_io::MessageItem> converse(model_io::AgentInputState state) override
    {
        called = true;
        for (const auto* name : {"memory_search", "memory_fetch"}) {
            BOOST_TEST(std::any_of(state.tools.begin(), state.tools.end(), [&](const auto& tool) {
                return tool.name == name;
            }) == expected);
        }
        const auto prompt = state.system_prompt.render().markdown;
        BOOST_TEST((prompt.find("Searching archived memory") != std::string::npos) == expected);
        BOOST_TEST(prompt.find("STALE ARCHIVE SKILL") == std::string::npos);
        model_io::MessageItem response;
        response.role = "assistant";
        response.type = model_io::MessageItemType::ModelResponse;
        model_io::Content text;
        text.raw = "done";
        response.content.push_back(std::move(text));
        if (retrieve && phase == 0) {
            model_io::InvokeQuery query;
            query.name = "memory_search";
            query.id = "search";
            query.arguments = {{"query", "old context"}};
            response.invokes = std::vector<model_io::InvokeQuery>{query};
        } else if (retrieve) {
            const auto& step = state.turns.back().agent_loop_step.back();
            BOOST_REQUIRE(step.invoke_returns);
            BOOST_REQUIRE_EQUAL(step.invoke_returns->size(), 1u);
            const auto& result = step.invoke_returns->front();
            BOOST_REQUIRE(!result.content.empty());
            const auto& raw = result.content.front().raw;
            if (phase == 1) {
                const auto locator = [&](const std::string& key) {
                    const auto marker = "[[" + key + "]]: ";
                    const auto begin = raw.find(marker);
                    BOOST_REQUIRE(begin != std::string::npos);
                    const auto start = begin + marker.size();
                    return raw.substr(start, raw.find('\n', start) - start);
                };
                model_io::InvokeQuery query;
                query.name = "memory_fetch";
                query.id = "fetch";
                query.arguments = {{"archive_id", locator("archive_id")}, {"source_id", locator("source_id")}};
                response.invokes = std::vector<model_io::InvokeQuery>{query};
            } else {
                BOOST_TEST(raw.find("old context: deployment checklist") != std::string::npos);
            }
        }
        ++phase;
        co_return response;
    }
};

/** Exercise startup through a real local peer, with no external model or Hub. */
void scenario(std::string backend, std::optional<persistence::Backend> existing,
              bool enabled, bool restore, bool expected, bool retrieve = false)
{
    const auto root = std::filesystem::temp_directory_path() / ("simplex-memory-core-" + core::new_identity());
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); }
    } cleanup{root};
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    load::Configuration config;
    config.directory = root;
    config.storage = root;
    config.state_directory = root / "state";
    config.memory = root / "memory";
    config.persistence_backend = std::move(backend);
    config.persistence = enabled;
    config.restore = restore;
    config.provider = "fixture";
    config.document = Json::object();
    config.client = load::websocket_endpoint("ws://127.0.0.1:"
        + std::to_string(acceptor.local_endpoint().port()) + "/events");
    if (existing) {
        auto store = *existing == persistence::Backend::Sqlite
            ? persistence::make_sqlite_store("test", config.state_directory)
            : persistence::make_json_store("test", {config.state_directory, config.memory});
        model_io::AgentInputState old;
        old.meta.session_id = "test";
        old.system_prompt.add_section("skill.archive_memory", "old", "STALE ARCHIVE SKILL");
        store->checkpoint(old, persistence::Boundary::RunFinished);
        if (retrieve) {
            model_io::UserLoopStep turn;
            turn.user_input.type = model_io::MessageItemType::UserInput;
            turn.user_input.role = "user";
            model_io::Content text;
            text.raw = "old context: deployment checklist";
            turn.user_input.content.push_back(std::move(text));
            old.turns.push_back(std::move(turn));
            const auto ref = store->reserve_archive({"historical"});
            store->begin_archive(ref, old);
            store->finish_archive_attempt(ref, persistence::ArchiveOutcome::Failed);
        }
    }
    auto model = std::make_shared<Model>(io.get_executor(), expected, retrieve);
    core::Application app(io.get_executor(), config, "test", model);
    bool completed = false;
    auto peer = [&]() -> asio::awaitable<void> {
        beast::websocket::stream<asio::ip::tcp::socket> socket(
            co_await acceptor.async_accept(asio::use_awaitable));
        co_await socket.async_accept(asio::use_awaitable);
        for (;;) {
            beast::flat_buffer buffer;
            boost::system::error_code error;
            co_await socket.async_read(buffer, asio::redirect_error(asio::use_awaitable, error));
            if (error) break;
            const auto event = Json::parse(beast::buffers_to_string(buffer.data()));
            if (event["event"] == "ready") {
                const auto wire = Json{{"type", "payload"}, {"data", {
                    {"operation", "message"}, {"request_id", "inspect"},
                    {"content", Json::array({{{"type", "text"}, {"raw", "hello"}, {"modality", "text"}}})}
                }}}.dump();
                co_await socket.async_write(asio::buffer(wire), asio::use_awaitable);
            }
            if (event["event"] == "run_finished") {
                BOOST_TEST(event["data"]["status"] == "completed");
                completed = true;
                app.stop();
            }
            if (event["event"] == "input_rejected" || event["event"] == "error") {
                app.stop();
                BOOST_ERROR(event.dump());
            }
        }
    };
    asio::steady_timer watchdog(io, std::chrono::seconds(5));
    watchdog.async_wait([&](auto error) { if (!error) { app.stop(); acceptor.cancel(); } });
    auto server = asio::co_spawn(io, peer(), asio::use_future);
    auto worker = asio::co_spawn(io, app.run(), asio::use_future);
    // Poll only the worker's completion; the watchdog bounds startup failures.
    while (worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready) io.run_one();
    watchdog.cancel();
    acceptor.cancel();
    io.run();
    worker.get();
    server.get();
    BOOST_TEST(completed);
    BOOST_TEST(model->called);
    if (retrieve) BOOST_TEST(model->phase == 3u);
}
} // namespace

BOOST_AUTO_TEST_CASE(explicit_sqlite_enables_archive_tools) {
    scenario("sqlite", {}, true, true, true);
}
BOOST_AUTO_TEST_CASE(explicit_json_omits_archive_tools) {
    scenario("json", {}, true, true, false);
}
BOOST_AUTO_TEST_CASE(auto_empty_directory_selects_sqlite) {
    scenario("auto", {}, true, true, true);
}
BOOST_AUTO_TEST_CASE(auto_existing_json_discards_stale_archive_skill) {
    scenario("auto", persistence::Backend::Json, true, true, false);
}
BOOST_AUTO_TEST_CASE(auto_existing_sqlite_exposes_archive_tools) {
    scenario("auto", persistence::Backend::Sqlite, true, true, true);
}
BOOST_AUTO_TEST_CASE(disabled_persistence_omits_tools_even_with_sqlite_configured) {
    scenario("sqlite", {}, false, true, false);
}
BOOST_AUTO_TEST_CASE(restore_never_does_not_disable_sqlite_archive_tools) {
    scenario("auto", persistence::Backend::Sqlite, true, false, true);
}

BOOST_AUTO_TEST_CASE(model_searches_and_fetches_archived_context_through_the_loop) {
    scenario("auto", persistence::Backend::Sqlite, true, true, true, true);
}
