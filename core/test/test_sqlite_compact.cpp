#define BOOST_TEST_MODULE CoreSqliteCompact
#include <boost/test/unit_test.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast.hpp>
#include "core/application.hpp"
#include "core/protocol.hpp"
#include "persistence/store.hpp"
#include <thread>
#include <fstream>
#include "core/compact.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
using Json = nlohmann::json;

/** The peer holds this model suspended while it queries status/history. */
struct SummaryModel : llm::LLMModel {
    explicit SummaryModel(asio::any_io_executor executor) : LLMModel(executor, {}) {}
    bool release = false;
    bool entered = false;
    llm::LLMModelType model_type() const noexcept override { return llm::LLMModelType::Conversation; }
    asio::awaitable<model_io::MessageItem> converse(model_io::AgentInputState state) override {
        BOOST_TEST(state.tools.empty());
        entered = true;
        while (!release) {
            asio::steady_timer wait(co_await asio::this_coro::executor, std::chrono::milliseconds(1));
            co_await wait.async_wait(asio::use_awaitable);
        }
        model_io::MessageItem response;
        response.role = "assistant";
        response.type = model_io::MessageItemType::ModelResponse;
        model_io::Content content;
        content.raw = "Goal: finish the user task. Completed: inspection. Next: verify the result."
            + std::string(64 * 1024, 'S');
        response.content.push_back(content);
        response.cost = model_io::TokenCost{100, 10, 0};
        co_return response;
    }
};

BOOST_AUTO_TEST_CASE(manual_sqlite_compact_keeps_history_live_and_preserves_full_summary) {
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    const auto root = std::filesystem::temp_directory_path() / ("simplex-sqlite-compact-" + core::new_identity());
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    load::Configuration config;
    config.directory = root;
    config.document = Json::object();
    config.provider = "fixture";
    config.client = load::websocket_endpoint("ws://127.0.0.1:"
        + std::to_string(acceptor.local_endpoint().port()) + "/events");
    config.storage = root;
    config.state_directory = root / "state";
    config.memory = root / "memory";
    config.persistence_backend = "sqlite";
    config.compact_prompt = "Summarize; do not call tools.";
    model_io::AgentInputState original;
    original.meta.session_id = "test";
    original.loop.emplace();
    model_io::UserLoopStep turn;
    turn.user_input.type = model_io::MessageItemType::UserInput;
    turn.user_input.role = "user";
    model_io::Content content;
    content.raw = std::string(16000, 'a');
    turn.user_input.content.push_back(content);
    original.turns.push_back(turn);
    auto store = persistence::make_sqlite_store("test", config.state_directory);
    store->checkpoint(original, persistence::Boundary::RunFinished);
    auto model = std::make_shared<SummaryModel>(io.get_executor());
    core::Application app(io.get_executor(), config, "test", model);
    bool completed = false;
    auto peer = [&]() -> asio::awaitable<void> {
        beast::websocket::stream<asio::ip::tcp::socket> socket(co_await acceptor.async_accept(asio::use_awaitable));
        co_await socket.async_accept(asio::use_awaitable);
        auto send = [&](const char* type, Json data) -> asio::awaitable<void> {
            const auto text = Json{{"type", type}, {"data", data}}.dump();
            co_await socket.async_write(asio::buffer(text), asio::use_awaitable);
        };
        for (;;) {
            beast::flat_buffer buffer;
            boost::system::error_code error;
            co_await socket.async_read(buffer, asio::redirect_error(asio::use_awaitable, error));
            if (error) break;
            const auto event = Json::parse(beast::buffers_to_string(buffer.data()));
            const auto name = event.at("event").get<std::string>();
            if (name == "ready") co_await send("payload", {{"operation", "compact"}, {"request_id", "compact"}});
            if (name == "run_started") {
                BOOST_TEST(store->load_current()->state.turns.size() == 1u);
                co_await send("payload", {{"operation", "history"}, {"request_id", "history"}});
            }
            if (name == "history") {
                BOOST_TEST(event["data"]["turns"].size() == 1u);
                if (model->entered) {
                    model->release = true;
                } else {
                    asio::steady_timer wait(io, std::chrono::milliseconds(1));
                    co_await wait.async_wait(asio::use_awaitable);
                    co_await send("payload", {{"operation", "history"}, {"request_id", core::new_identity()}});
                }
            }
            if (name == "compact_finished") {
                completed = true;
                BOOST_CHECK(!event["data"].contains("memory_file"));
                BOOST_TEST(event["data"]["archive"]["storage_kind"] == "sqlite");
                BOOST_TEST(event["data"]["archive"]["restorable"] == true);
                const auto saved = store->load_current();
                BOOST_TEST(saved->state.turns.empty());
                const auto prompt = saved->state.system_prompt.render().markdown;
                const auto preview = event["data"]["summary"].get<std::string>();
                BOOST_CHECK(preview.size() <= core::compact_summary_max_bytes);
                BOOST_CHECK(preview.find("Summary truncated") != std::string::npos);
                BOOST_CHECK(prompt.find(preview) != std::string::npos);
                BOOST_CHECK(preview.find((root / "memory.md").string()) != std::string::npos);
                std::ifstream file(root / "memory.md");
                const std::string full{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
                BOOST_TEST(full == "Goal: finish the user task. Completed: inspection. Next: verify the result." + std::string(64 * 1024, 'S'));
                BOOST_TEST(prompt.find("use reading tools in") == std::string::npos);
                BOOST_CHECK(!std::filesystem::exists(config.memory));
            }
            if (name == "run_finished") {
                BOOST_TEST_CONTEXT(event.dump()) {
                    BOOST_TEST(event["data"]["status"] == "completed");
                }
                co_await send("signal", {{"operation", "shutdown"}});
            }
            if (name == "error" || name == "input_rejected") {
                app.stop();
                BOOST_FAIL(event.dump());
            }
        }
    };
    auto server = asio::co_spawn(io, peer(), asio::use_future);
    auto worker = asio::co_spawn(io, app.run(), asio::use_future);
    asio::steady_timer watchdog(io, std::chrono::seconds(10));
    watchdog.async_wait([&](auto error) { if (!error) app.stop(); });
    std::jthread runner([&] { io.run(); });
    std::exception_ptr failure;
    try {
        worker.get();
        server.get();
    } catch (...) {
        failure = std::current_exception();
        asio::post(io, [&] { acceptor.cancel(); io.stop(); });
    }
    asio::post(io, [&] { watchdog.cancel(); });
    runner.join();
    if (failure) std::rethrow_exception(failure);
    BOOST_TEST(completed);
}
