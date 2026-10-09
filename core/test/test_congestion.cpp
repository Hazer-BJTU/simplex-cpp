#define BOOST_TEST_MODULE CoreCongestion
#include <boost/test/unit_test.hpp>
#include "core/application.hpp"
#include "core/protocol.hpp"
#include "load/persistence.hpp"
#include <boost/asio/use_future.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/beast.hpp>
#include <atomic>
#include <thread>

namespace asio = boost::asio;
namespace beast = boost::beast;
using Json = nlohmann::json;

namespace {
/** Persist many real loop checkpoints, then suspend at a cancellable model wait. */
struct Model : llm::LLMModel {
    explicit Model(asio::any_io_executor executor) : LLMModel(executor, {}) {}
    llm::LLMModelType model_type() const noexcept override { return llm::LLMModelType::Conversation; }
    std::atomic<int> calls{0};
    std::atomic<bool> waiting{false}, cancelled{false};
    asio::awaitable<model_io::MessageItem> converse(model_io::AgentInputState) override {
        const auto number = ++calls;
        if (number <= 40) {
            // Give the writer opportunities to drain until the non-reading
            // peer's receive window actually imposes transport backpressure.
            asio::steady_timer tick(co_await asio::this_coro::executor, std::chrono::milliseconds(1));
            co_await tick.async_wait(asio::use_awaitable);
        }
        if (number == 41) {
            waiting = true;
            asio::steady_timer timer(co_await asio::this_coro::executor, std::chrono::seconds(20));
            try { co_await timer.async_wait(asio::use_awaitable); }
            catch (const boost::system::system_error&) { cancelled = true; throw; }
        }
        model_io::MessageItem response;
        response.role = "assistant";
        response.type = model_io::MessageItemType::ModelResponse;
        model_io::Content text;
        text.raw = number <= 40 ? std::string(128 * 1024, 'R') : "Recovered after congestion.";
        response.content.push_back(std::move(text));
        if (number <= 40) {
            model_io::InvokeQuery query;
            query.id = "tool-" + std::to_string(number);
            query.name = "unregistered_test_tool";
            query.arguments = Json::object();
            response.invokes = std::vector<model_io::InvokeQuery>{std::move(query)};
        }
        co_return response;
    }
};

/** Two ordinary final answers supply realistic history for continuous polling. */
struct PollingModel : llm::LLMModel {
    explicit PollingModel(asio::any_io_executor executor) : LLMModel(executor, {}) {}
    llm::LLMModelType model_type() const noexcept override {
        return llm::LLMModelType::Conversation;
    }
    int calls = 0;
    asio::awaitable<model_io::MessageItem> converse(model_io::AgentInputState) override {
        ++calls;
        asio::steady_timer timer(co_await asio::this_coro::executor,
            std::chrono::milliseconds(20));
        co_await timer.async_wait(asio::use_awaitable);
        model_io::MessageItem response;
        response.role = "assistant";
        response.type = model_io::MessageItemType::ModelResponse;
        model_io::Content text;
        text.raw = std::string(128 * 1024, 'A');
        response.content.push_back(std::move(text));
        co_return response;
    }
};
}

BOOST_AUTO_TEST_CASE(stalled_peer_tool_steps_query_flood_cancel_and_terminal_recovery) {
    const auto root = std::filesystem::temp_directory_path() / ("simplex-congestion-" + core::new_identity());
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    } cleanup{root};
    asio::io_context context;
    asio::ip::tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    load::Configuration config;
    config.document = Json::object();
    config.provider = "fixture";
    config.directory = root;
    config.storage = root / "session";
    config.state_directory = config.storage / "state";
    config.event_capacity = 2;
    config.transport.write_capacity = 1;
    config.queues.query_capacity = 2;
    config.queues.signal_capacity = 2;
    config.client = load::websocket_endpoint("ws://127.0.0.1:"
        + std::to_string(acceptor.local_endpoint().port()) + "/events");
    auto model = std::make_shared<Model>(context.get_executor());
    core::Application app(context.get_executor(), config, "test", model);
    auto peer = [&]() -> asio::awaitable<void> {
        beast::websocket::stream<asio::ip::tcp::socket> socket(
            co_await acceptor.async_accept(asio::use_awaitable));
        socket.next_layer().set_option(asio::socket_base::receive_buffer_size(64 * 1024));
        co_await socket.async_accept(asio::use_awaitable);
        auto send = [&](std::string type, Json data) -> asio::awaitable<void> {
            const auto wire = Json{{"type", type}, {"data", std::move(data)}}.dump();
            co_await socket.async_write(asio::buffer(wire), asio::use_awaitable);
        };
        auto input = [&](std::string id) -> asio::awaitable<void> {
            co_await send("payload", {{"operation", "message"}, {"request_id", id},
                {"content", Json::array({{{"type", "text"}, {"modality", "text"}, {"raw", "Run the test."}}})}});
        };
        std::uint64_t sequence = 0;
        std::size_t previews = 0;
        bool finished = false;
        for (;;) {
            beast::flat_buffer buffer;
            boost::system::error_code error;
            co_await socket.async_read(buffer, asio::redirect_error(asio::use_awaitable, error));
            if (error) break;
            auto event = Json::parse(beast::buffers_to_string(buffer.data()));
            BOOST_TEST(event.at("sequence").get<std::uint64_t>() == ++sequence);
            const auto name = event.at("event").get<std::string>();
            if (event.at("request_id") == "first"
                && (name == "model_response" || name == "tool_calls" || name == "tool_results")) ++previews;
            if (name == "ready") co_await input("first");
            if (name == "input_admitted" && event.at("request_id") == "first") {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                while (!model->waiting && std::chrono::steady_clock::now() < deadline) {
                    asio::steady_timer tick(context, std::chrono::milliseconds(5));
                    co_await tick.async_wait(asio::use_awaitable);
                }
                BOOST_REQUIRE(model->waiting);
                // Do not read output while a large tool/checkpoint stream and
                // excess read-only queries accumulate behind the blocked write.
                for (int i = 0; i < 300; ++i) {
                    co_await send("payload", {{"operation", i % 2 ? "history" : "answer"},
                        {"request_id", "query-" + std::to_string(i)}});
                }
                co_await send("signal", {{"operation", "cancel"}, {"run_id", event.at("run_id")}});
                const auto cancel_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while (!model->cancelled && std::chrono::steady_clock::now() < cancel_deadline) {
                    asio::steady_timer tick(context, std::chrono::milliseconds(5));
                    co_await tick.async_wait(asio::use_awaitable);
                }
                BOOST_REQUIRE(model->cancelled);
                socket.next_layer().set_option(asio::socket_base::receive_buffer_size(256 * 1024));
            }
            if (name == "run_finished" && event.at("request_id") == "first") {
                BOOST_TEST(event.at("data").at("status") == "cancelled");
                BOOST_TEST(event.at("data").at("omitted_display_events").get<std::size_t>() > 0u);
                BOOST_TEST(event.at("data").at("omitted_display_events").get<std::size_t>() + previews == 120u);
                BOOST_TEST(event.at("data").at("coalesced_metadata_events").get<std::size_t>() > 0u);
                const auto state = load::load_state(config.state_directory / "state.json");
                BOOST_TEST(state.turns.back().agent_loop_step.size() == 40u);
                BOOST_CHECK(state.loop->status == model_io::LoopStatus::Cancelled);
                finished = true;
                co_await input("second");
            } else if (name == "run_finished" && event.at("request_id") == "second") {
                BOOST_TEST(event.at("data").at("status") == "completed");
                co_await send("signal", {{"operation", "shutdown"}});
            }
        }
        BOOST_CHECK(finished);
        BOOST_TEST(model->calls.load() == 42);
    };
    auto server = asio::co_spawn(context, peer, asio::use_future);
    auto worker = asio::co_spawn(context, app.run(), asio::use_future);
    asio::steady_timer watchdog(context, std::chrono::seconds(45));
    watchdog.async_wait([&](boost::system::error_code ec) { if (!ec) app.stop(); });
    std::jthread second([&] { context.run(); });
    // Cancel the watchdog as soon as both supervised futures have settled.
    std::jthread joiner([&] { worker.wait(); asio::post(context, [&] { watchdog.cancel(); }); });
    context.run();
    second.join(); joiner.join();
    server.get(); worker.get();
}

BOOST_AUTO_TEST_CASE(continuous_history_and_status_polling_cannot_starve_second_request_admission) {
    asio::io_context context;
    asio::ip::tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    load::Configuration config;
    config.document = Json::object();
    config.directory = std::filesystem::temp_directory_path();
    config.provider = "fixture";
    config.persistence = false;
    config.event_capacity = 4;
    config.transport.write_capacity = 1;
    config.queues.query_capacity = 64;
    config.client = load::websocket_endpoint("ws://127.0.0.1:"
        + std::to_string(acceptor.local_endpoint().port()) + "/events");
    auto model = std::make_shared<PollingModel>(context.get_executor());
    core::Application app(context.get_executor(), config, "test", model);
    auto peer = [&]() -> asio::awaitable<void> {
        beast::websocket::stream<asio::ip::tcp::socket> socket(
            co_await acceptor.async_accept(asio::use_awaitable));
        co_await socket.async_accept(asio::use_awaitable);
        auto send = [&](std::string type, Json data) -> asio::awaitable<void> {
            const auto wire = Json{{"type", type}, {"data", std::move(data)}}.dump();
            co_await socket.async_write(asio::buffer(wire), asio::use_awaitable);
        };
        auto input = [&](const std::string& id) -> asio::awaitable<void> {
            co_await send("payload", {{"operation", "message"}, {"request_id", id},
                {"content", Json::array({{{"type", "text"}, {"modality", "text"},
                    {"raw", "Answer while the panel keeps polling."}}})}});
        };
        bool polling = false;
        bool polling_started = false;
        bool second_admitted = false;
        bool second_finished = false;
        std::size_t queries = 0;
        std::size_t histories = 0;
        std::size_t statuses = 0;
        std::chrono::steady_clock::time_point submitted;
        asio::experimental::channel<void(boost::system::error_code, std::exception_ptr)>
            polling_done(context, 1);
        auto poll = [&]() -> asio::awaitable<void> {
            // Seed output before the next input, then continue producing queries
            // independently of the reader. There is exactly one socket writer.
            for (int i = 0; i < 64; ++i) {
                co_await send("payload", {{"operation", "history"},
                    {"request_id", "query-" + std::to_string(++queries)}});
            }
            submitted = std::chrono::steady_clock::now();
            co_await input("second");
            const auto deadline = submitted + std::chrono::seconds(5);
            while (!second_admitted && std::chrono::steady_clock::now() < deadline) {
                for (int i = 0; i < 8; ++i) {
                    co_await send("payload", {{"operation", "history"},
                        {"request_id", "query-" + std::to_string(++queries)}});
                }
                co_await send("signal", {{"operation", "status"}});
                asio::steady_timer tick(context, std::chrono::milliseconds(1));
                co_await tick.async_wait(asio::use_awaitable);
            }
            polling = false;
            // The bounded assertion concerns admission under ongoing polling.
            // Once admitted, let the model finish and drain the finite backlog
            // before shutdown rather than cancelling it at that same deadline.
            const auto completion_deadline = std::chrono::steady_clock::now()
                + std::chrono::seconds(10);
            while (second_admitted && !second_finished
                   && std::chrono::steady_clock::now() < completion_deadline) {
                asio::steady_timer tick(context, std::chrono::milliseconds(5));
                co_await tick.async_wait(asio::use_awaitable);
            }
            co_await send("signal", {{"operation", "shutdown"}});
        };
        std::uint64_t sequence = 0;
        for (;;) {
            beast::flat_buffer buffer;
            boost::system::error_code error;
            co_await socket.async_read(buffer, asio::redirect_error(asio::use_awaitable, error));
            if (error) break;
            const auto event = Json::parse(beast::buffers_to_string(buffer.data()));
            BOOST_TEST(event.at("sequence").get<std::uint64_t>() == ++sequence);
            const auto name = event.at("event").get<std::string>();
            if (name == "ready") {
                co_await input("first");
            } else if (name == "history") {
                ++histories;
            } else if (name == "status") {
                ++statuses;
            } else if (name == "run_finished" && event.at("request_id") == "first") {
                BOOST_TEST(event.at("data").at("status") == "completed");
                polling = true;
                polling_started = true;
                asio::co_spawn(context, poll, [&](std::exception_ptr failure) {
                    polling_done.try_send(boost::system::error_code{}, failure);
                    if (failure) app.stop();
                });
            } else if (name == "input_admitted" && event.at("request_id") == "second") {
                BOOST_CHECK(polling);
                BOOST_CHECK(std::chrono::steady_clock::now() - submitted < std::chrono::seconds(5));
                second_admitted = true;
            } else if (name == "run_finished" && event.at("request_id") == "second") {
                BOOST_TEST(event.at("data").at("status") == "completed");
                second_finished = true;
            }
            // Keep reading throughout the test, but slower than the polling
            // producer so the pre-fix all-traffic gate cannot rely on idle gaps.
            asio::steady_timer tick(context, std::chrono::milliseconds(1));
            co_await tick.async_wait(asio::use_awaitable);
        }
        if (polling_started) {
            auto failure = co_await polling_done.async_receive(asio::use_awaitable);
            if (failure) std::rethrow_exception(failure);
        }
        BOOST_CHECK(second_admitted);
        BOOST_CHECK(second_finished);
        BOOST_TEST(queries > 64u);
        BOOST_TEST(histories > 0u);
        BOOST_TEST(statuses > 0u);
    };
    asio::steady_timer watchdog(context, std::chrono::seconds(20));
    watchdog.async_wait([&](boost::system::error_code error) {
        if (!error) app.stop();
    });
    std::exception_ptr worker_failure;
    asio::co_spawn(context, app.run(), [&](std::exception_ptr failure) {
        worker_failure = failure;
        watchdog.cancel();
        // A startup failure must also release a peer still awaiting connection.
        boost::system::error_code ignored;
        acceptor.close(ignored);
    });
    auto server = asio::co_spawn(context, peer, asio::use_future);
    context.run();
    server.get();
    if (worker_failure) std::rethrow_exception(worker_failure);
    BOOST_TEST(model->calls == 2);
}
