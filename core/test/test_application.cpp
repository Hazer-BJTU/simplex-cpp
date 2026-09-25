#define BOOST_TEST_MODULE CoreApplication
#include <boost/test/unit_test.hpp>
#include "core/application.hpp"
#include "load/persistence.hpp"
#include <boost/beast.hpp>
#include <boost/asio/use_future.hpp>
#include <atomic>
#include <fstream>
#include <thread>
#include <unistd.h>

namespace asio = boost::asio;
namespace beast = boost::beast;
using Json = nlohmann::json;
namespace {

/** Offline model with a cancellable wait; no tool or external endpoint runs. */
struct Model : llm::LLMModel {
    explicit Model(asio::any_io_executor executor) : LLMModel(executor, {}) {}
    llm::LLMModelType model_type() const noexcept override { return llm::LLMModelType::Conversation; }
    std::atomic<int> active{0};
    std::atomic<int> maximum{0};
    std::atomic<int> calls{0};
    std::chrono::milliseconds delay{40};

    asio::awaitable<model_io::MessageItem> converse(model_io::AgentInputState) override {
        ++calls;
        maximum.store(std::max(maximum.load(), ++active));
        struct Exit { std::atomic<int>& active; ~Exit() { --active; } } exit{active};
        asio::steady_timer timer(co_await asio::this_coro::executor, delay);
        co_await timer.async_wait(asio::use_awaitable);
        model_io::MessageItem item;
        item.type = model_io::MessageItemType::ModelResponse;
        item.role = "assistant";
        model_io::Content content;
        content.raw = "offline-answer";
        item.content.push_back(content);
        co_return item;
    }
};

struct Scratch {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / ("simplex_core_test_" + std::to_string(::getpid()));
    Scratch() { std::filesystem::remove_all(root); std::filesystem::create_directories(root); }
    ~Scratch() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};

enum class Mode { Normal, Cancel, Overflow, StorageFailure, Blocked, Stop, ProtocolFailure };

/** Real local WebSocket peer drives the complete worker lifecycle. */
void scenario(Mode mode) {
    Scratch scratch;
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    load::Configuration config;
    config.directory = scratch.root;
    config.document = Json::object();
    config.client = load::websocket_endpoint("ws://127.0.0.1:"
        + std::to_string(acceptor.local_endpoint().port()) + "/events");
    config.storage = scratch.root / "sessions";
    config.event_capacity = mode == Mode::Overflow ? 1 : 256;
    if (mode == Mode::StorageFailure) {
        std::filesystem::create_directories(config.storage / "test/state.json");
        config.restore = false;
    }
    if (mode == Mode::Blocked) {
        model_io::AgentInputState state;
        state.meta.session_id = "test";
        state.loop.emplace();
        state.loop->phase = model_io::LoopPhase::Blocked;
        load::save_state(config.storage / "test/state.json", state);
    }
    auto model = std::make_shared<Model>(io.get_executor());
    if (mode == Mode::Cancel || mode == Mode::Stop) model->delay = std::chrono::seconds(10);
    core::Application app(io.get_executor(), config, "test", model);
    int completed = 0;
    int rejected = 0;
    bool cancelled = false;
    auto peer = [&]() -> asio::awaitable<void> {
        beast::websocket::stream<asio::ip::tcp::socket> socket(
            co_await acceptor.async_accept(asio::use_awaitable));
        co_await socket.async_accept(asio::use_awaitable);
        auto send = [&](Json message) -> asio::awaitable<void> {
            auto wire = message.dump();
            co_await socket.async_write(asio::buffer(wire), asio::use_awaitable);
        };
        for (;;) {
            beast::flat_buffer buffer;
            boost::system::error_code error;
            co_await socket.async_read(buffer, asio::redirect_error(asio::use_awaitable, error));
            if (error) break;
            auto event = Json::parse(beast::buffers_to_string(buffer.data()));
            const auto name = event.at("event").get<std::string>();
            if (name == "ready") {
                if (mode == Mode::ProtocolFailure) {
                    socket.binary(true);
                    const std::string binary = "invalid application frame";
                    co_await socket.async_write(asio::buffer(binary), asio::use_awaitable);
                    continue;
                }
                Json input = {
                    {"type", "payload"},
                    {"data", {
                        {"operation", "message"},
                        {"request_id", "one"},
                        {"content", Json::array({{
                            {"type", "text"}, {"raw", "hello"}
                        }})}
                    }}
                };
                co_await send(input);
                if (mode == Mode::Normal) {
                    co_await send(input);
                    input["data"]["request_id"] = "two";
                    co_await send(input);
                }
            } else if (name == "run_started") {
                if (mode == Mode::Stop) {
                    std::thread stopper([&] { app.stop(); });
                    stopper.join();
                    continue;
                }
                Json signal = {{"type", "signal"}, {"data", {
                    {"operation", "cancel"}, {"run_id", mode == Mode::Cancel
                        ? event.at("run_id").get<std::string>() : "stale-run"}}}};
                co_await send(signal);
            } else if (name == "run_finished") {
                ++completed;
                cancelled = event["data"]["status"] == "cancelled";
                if (mode != Mode::Normal || completed == 2)
                    co_await send(Json{{"type", "signal"}, {"data", {{"operation", "shutdown"}}}});
            } else if (name == "input_rejected") {
                ++rejected;
                if (mode == Mode::Blocked)
                    co_await send(Json{{"type", "signal"}, {"data", {{"operation", "shutdown"}}}});
            }
        }
    };
    auto server = asio::co_spawn(io, peer, asio::use_future);
    auto worker = asio::co_spawn(io, app.run(), asio::use_future);
    std::thread other([&] { io.run(); });
    io.run();
    other.join();
    server.get();
    if (mode == Mode::ProtocolFailure) {
        BOOST_CHECK_THROW(worker.get(), intercom::WsProtocolException);
    } else if (mode == Mode::Overflow || mode == Mode::StorageFailure) {
        BOOST_CHECK_THROW(worker.get(), std::exception);
    } else {
        worker.get();
        if (mode == Mode::Normal) {
            BOOST_TEST(completed == 2);
            BOOST_TEST(rejected == 1);
            BOOST_TEST(model->maximum.load() == 1);
            BOOST_TEST(model->calls.load() == 2);
            const auto state = load::load_state(config.storage / "test/state.json");
            BOOST_TEST(state.turns.size() == 2u);
            BOOST_TEST(!state.meta.created_at.empty());
            BOOST_TEST(!state.meta.updated_at.empty());
        } else if (mode == Mode::Cancel || mode == Mode::Stop) {
            if (mode == Mode::Cancel) BOOST_TEST(cancelled);
            const auto state = load::load_state(config.storage / "test/state.json");
            BOOST_CHECK(state.loop->status == model_io::LoopStatus::Cancelled);
        } else {
            BOOST_TEST(rejected == 1);
            BOOST_TEST(model->calls.load() == 0);
        }
    }
    if (mode == Mode::Normal) {
        io.restart();
        core::Application restored(io.get_executor(), config, "test", model);
        std::stop_source stop;
        stop.request_stop();
        auto restart = asio::co_spawn(io, restored.run(stop.get_token()), asio::use_future);
        io.run();
        restart.get();
        BOOST_TEST(load::load_state(config.storage / "test/state.json").turns.size() == 2u);
    }
    io.restart();
    auto twice = asio::co_spawn(io, app.run(), asio::use_future);
    io.run();
    BOOST_CHECK_THROW(twice.get(), std::logic_error);
}
}
BOOST_AUTO_TEST_CASE(serial_admission_duplicate_rejection_and_stale_cancel) { scenario(Mode::Normal); }
BOOST_AUTO_TEST_CASE(model_cancellation_saves_settled_state) { scenario(Mode::Cancel); }
BOOST_AUTO_TEST_CASE(cross_thread_stop_drains_active_model) { scenario(Mode::Stop); }
BOOST_AUTO_TEST_CASE(event_overflow_stops_worker) { scenario(Mode::Overflow); }
BOOST_AUTO_TEST_CASE(required_snapshot_failure_stops_admission) { scenario(Mode::StorageFailure); }
BOOST_AUTO_TEST_CASE(blocked_restore_never_executes_model) { scenario(Mode::Blocked); }

BOOST_AUTO_TEST_CASE(protocol_failure_survives_payload_queue_shutdown) { scenario(Mode::ProtocolFailure); }

BOOST_AUTO_TEST_CASE(startup_failure_releases_ownership_while_application_survives) {
    Scratch scratch;
    asio::io_context io;
    load::Configuration config;
    config.directory = scratch.root;
    config.document = Json::object();
    config.client = load::websocket_endpoint("ws://127.0.0.1:1/events");
    config.storage = scratch.root / "sessions";
    const auto snapshot = config.storage / "test/state.json";
    std::filesystem::create_directories(snapshot.parent_path());
    std::ofstream(snapshot) << "invalid json";
    auto model = std::make_shared<Model>(io.get_executor());
    core::Application broken(io.get_executor(), config, "test", model);
    auto failed = asio::co_spawn(io, broken.run(), asio::use_future);
    io.run();
    BOOST_CHECK_THROW(failed.get(), std::exception);

    // Keep broken alive: release must be scoped to run(), not Impl destruction.
    std::filesystem::remove(snapshot);
    io.restart();
    core::Application repaired(io.get_executor(), config, "test", model);
    std::stop_source stop;
    stop.request_stop();
    auto result = asio::co_spawn(io, repaired.run(stop.get_token()), asio::use_future);
    io.run();
    BOOST_CHECK_NO_THROW(result.get());
}

namespace {

/** Provider-owned option metadata, or a failure, without any remote catalogue IO. */
struct OptionsModel : Model {
    OptionsModel(asio::any_io_executor executor, Json choices, bool fail)
        : Model(executor), choices(std::move(choices)), fail(fail) {
        delay = std::chrono::seconds(10);
    }

    Json get_options() const override {
        if (fail) {
            throw std::runtime_error("option discovery failed");
        }
        return choices;
    }

    const Json choices;
    const bool fail;
};

/** Query over real text WebSockets while idle and during a cancellable run. */
void options_scenario(Json choices, bool fail = false) {
    Scratch scratch;
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    load::Configuration config;
    config.directory = scratch.root;
    config.document = Json::object();
    config.persistence = false;
    config.client = load::websocket_endpoint(
        "ws://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/events");
    auto model = std::make_shared<OptionsModel>(io.get_executor(), choices, fail);
    core::Application app(io.get_executor(), config, "test", model);
    int replies = 0;
    std::string identity;
    std::string run;
    std::uint64_t sequence = 0;
    auto peer = [&]() -> asio::awaitable<void> {
        beast::websocket::stream<asio::ip::tcp::socket> socket(
            co_await acceptor.async_accept(asio::use_awaitable));
        co_await socket.async_accept(asio::use_awaitable);
        auto send = [&](Json message) -> asio::awaitable<void> {
            const auto wire = message.dump();
            co_await socket.async_write(asio::buffer(wire), asio::use_awaitable);
        };
        for (;;) {
            beast::flat_buffer buffer;
            boost::system::error_code error;
            co_await socket.async_read(buffer, asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                break;
            }
            const auto event = Json::parse(beast::buffers_to_string(buffer.data()));
            BOOST_TEST(event.at("type") == "event");
            BOOST_TEST(event.at("session_id") == "test");
            const auto next = event.at("sequence").get<std::uint64_t>();
            BOOST_TEST(next > sequence);
            sequence = next;
            const auto name = event.at("event").get<std::string>();
            if (name == "ready") {
                identity = event.at("worker_id").get<std::string>();
                BOOST_TEST(!identity.empty());
                co_await send({{"type", "signal"}, {"data", {{"operation", "options"}}}});
            } else if (name == "options") {
                BOOST_TEST(!fail);
                BOOST_TEST(event.at("worker_id") == identity);
                BOOST_TEST(event.at("data") == Json({
                    {"model", choices}, {"tools", Json::array()},
                    {"confirmation", Json::array()}
                }));
                ++replies;
                if (replies == 1) {
                    BOOST_TEST(event.at("request_id") == "");
                    BOOST_TEST(event.at("run_id") == "");
                    BOOST_TEST(model->calls.load() == 0);
                    co_await send({{"type", "payload"}, {"data", {
                        {"operation", "message"}, {"request_id", "options-run"},
                        {"content", Json::array({{{"type", "text"}, {"raw", "hello"}}})}
                    }}});
                } else {
                    BOOST_TEST(event.at("request_id") == "options-run");
                    BOOST_TEST(event.at("run_id") == run);
                    co_await send({{"type", "signal"}, {"data", {
                        {"operation", "cancel"}, {"run_id", run}
                    }}});
                }
            } else if (name == "run_started") {
                run = event.at("run_id").get<std::string>();
                co_await send({{"type", "signal"}, {"data", {{"operation", "options"}}}});
            } else if (name == "error") {
                BOOST_TEST(fail);
                BOOST_TEST(event.at("data").at("message") == "option discovery failed");
                ++replies;
                co_await send({{"type", "signal"}, {"data", {{"operation", "status"}}}});
            } else if (name == "run_finished" || (fail && name == "status")) {
                if (fail) {
                    BOOST_TEST(event.at("data").at("active") == false);
                } else {
                    BOOST_TEST(event.at("data").at("status") == "cancelled");
                }
                co_await send({{"type", "signal"}, {"data", {{"operation", "shutdown"}}}});
            }
        }
    };
    auto server = asio::co_spawn(io, peer, asio::use_future);
    auto worker = asio::co_spawn(io, app.run(), asio::use_future);
    std::jthread other([&] { io.run(); });
    io.run();
    other.join();
    server.get();
    worker.get();
    BOOST_TEST(replies == (fail ? 1 : 2));
    if (fail) {
        BOOST_TEST(model->calls.load() == 0);
    }
}

} // namespace

BOOST_AUTO_TEST_CASE(options_signal_returns_categories_and_run_metadata) {
    options_scenario(Json::array({{
        {"name", "model"}, {"options", {"fixture-a", "fixture-b"}}
    }}));
}

BOOST_AUTO_TEST_CASE(options_signal_preserves_an_empty_provider_list) {
    options_scenario(Json::array());
}

BOOST_AUTO_TEST_CASE(options_failure_reports_error_and_keeps_worker_available) {
    options_scenario(Json::array(), true);
}
