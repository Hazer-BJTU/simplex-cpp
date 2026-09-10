/**
 * @file test_chat_completions_concurrency.cpp
 * @brief The reentrancy contract of converse(): several exchanges in flight on
 *        ONE model instance, on a multi-threaded executor.
 *
 * What each case pins (see the LLMModel class doc for the contract itself):
 *
 *   - concurrent exchanges do not cross-contaminate: each returns the answer
 *     to ITS OWN request, and each request reaches the wire intact;
 *   - a concurrent set_generation() cannot tear an in-flight exchange — every
 *     request body carries one coherent snapshot of the knobs, never a mix of
 *     the pre- and post-patch values;
 *   - live events stay demultiplexable: one process-unique exchange_id per
 *     exchange, repeated on the returned MessageItem's extras, so a subscriber
 *     can bind a stream to its result;
 *   - the by-value AgentInputState survives the temporary-argument trap that a
 *     reference parameter would turn into a dangling read.
 *
 * The server is genuinely concurrent (one thread per connection, all held open
 * until every expected exchange has arrived), so the exchanges really do
 * overlap rather than queueing behind one another.
 */

#define BOOST_TEST_MODULE chat_completions_concurrency
#include <boost/test/unit_test.hpp>

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "eventbus/event_bus.hpp"
#include "llm/chat_completions/events.hpp"
#include "llm/chat_completions/model.hpp"
#include "loopback_server.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

namespace {

class FixtureDialect final : public llm::chat_completions::ChatCompletionsDialect {
public:
    std::string_view provider_name() const override { return "fixture"; }
};

class FixtureModel final : public llm::chat_completions::ChatCompletionsModel {
public:
    FixtureModel(asio::any_io_executor executor, nlohmann::json config)
        : ChatCompletionsModel(std::move(executor), std::move(config),
                               std::make_shared<const FixtureDialect>()) {}
};

std::string sse(const nlohmann::json& chunk) {
    return "data: " + chunk.dump() + "\n\n";
}

/// One scripted stream: a reasoning increment then a content increment, both
/// tagged with @p marker so the assertions can tell whose answer this was.
std::string scripted_stream(const std::string& marker) {
    const auto frame = [&](nlohmann::json delta,
                           nlohmann::json finish = nullptr) {
        return nlohmann::json{
            {"id", "chatcmpl-" + marker},
            {"object", "chat.completion.chunk"},
            {"model", "fixture-model"},
            {"choices", nlohmann::json::array({{
                {"index", 0},
                {"delta", std::move(delta)},
                {"finish_reason", std::move(finish)},
            }})},
        };
    };
    return sse(frame({{"role", "assistant"},
                      {"reasoning_content", "think-" + marker}})) +
           sse(frame({{"content", "answer-" + marker}})) +
           sse(frame(nlohmann::json::object(), "stop")) +
           "data: [DONE]\n\n";
}

/**
 * A loopback server that holds N connections open SIMULTANEOUSLY.
 *
 * The existing loopback fixtures serve one connection at a time, which would
 * quietly serialise the very overlap these tests are about. Here each accepted
 * connection gets its own thread, and every one of them waits at a rendezvous
 * until all @p expected have arrived before answering — so the exchanges are
 * provably concurrent rather than merely spawned together.
 *
 * The reply echoes a marker taken from the request body, which is what lets
 * the assertions prove answer-to-request pairing instead of assuming it.
 */
class ConcurrentServer {
public:
    /// @param expected  How many connections to accept and hold open together.
    /// @param mark      Reads the request body, returns this exchange's marker.
    explicit ConcurrentServer(std::size_t expected,
                              std::function<std::string(const std::string&)> mark)
        : _expected(expected), _mark(std::move(mark)) {
        _thread = std::thread([this] { _run(); });
    }

    ~ConcurrentServer() { join(); }

    unsigned short wait_listening() { return _port.get_future().get(); }

    /// Joining the acceptor thread is enough: it owns _workers and joins them
    /// itself before returning, so no other thread ever touches that vector.
    void join() {
        if (_thread.joinable()) _thread.join();
    }

    /// The request bodies, in arrival order (guarded: workers append).
    std::vector<std::string> captured() {
        std::lock_guard<std::mutex> lock(_mutex);
        return _captured;
    }

private:
    void _run() {
        // The io_context must outlive every worker: the sockets they hold are
        // bound to ITS services, so letting it die at the end of the accept
        // loop would be a use-after-free in each still-running worker. Hence
        // the join before this scope ends.
        asio::io_context io;
        asio::ip::tcp::acceptor acceptor(
            io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        _port.set_value(acceptor.local_endpoint().port());

        for (std::size_t accepted = 0; accepted < _expected; ++accepted) {
            auto socket = std::make_shared<asio::ip::tcp::socket>(io);
            boost::system::error_code ec;
            acceptor.accept(*socket, ec);
            if (ec) break;
            _workers.emplace_back([this, socket] { _serve(socket); });
        }
        for (auto& worker : _workers) {
            if (worker.joinable()) worker.join();
        }
    }

    void _serve(std::shared_ptr<asio::ip::tcp::socket> socket) {
        try {
            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            http::read(*socket, buffer, request);

            std::string marker;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _captured.push_back(request.body());
                marker = _mark(request.body());
            }

            // The rendezvous: nobody answers until everyone has asked, so all
            // the exchanges are in flight at the same time by construction.
            {
                std::unique_lock<std::mutex> lock(_gate_mutex);
                if (++_arrived >= _expected) {
                    _gate.notify_all();
                } else {
                    _gate.wait_for(lock, std::chrono::seconds(5),
                                   [this] { return _arrived >= _expected; });
                }
            }

            http::response<http::string_body> response(http::status::ok, 11);
            response.set(http::field::content_type, "text/event-stream");
            response.set(http::field::connection, "close");
            response.body() = scripted_stream(marker);
            response.prepare_payload();
            http::write(*socket, response);

            boost::system::error_code ignored;
            socket->shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
            socket->close(ignored);
        } catch (...) {
            // A failed worker shows up as a missing/failed exchange in the
            // test's own assertions; never let it terminate the process.
        }
    }

    std::size_t _expected;
    std::function<std::string(const std::string&)> _mark;
    std::promise<unsigned short> _port;
    std::thread _thread;
    std::vector<std::thread> _workers;
    std::mutex _mutex;
    std::vector<std::string> _captured;
    std::mutex _gate_mutex;
    std::condition_variable _gate;
    std::size_t _arrived = 0;
};

nlohmann::json loopback_config(unsigned short port) {
    return nlohmann::json{
        {"model", "fixture-model"},
        {"temperature", 0.1},
        {"retry", {{"max_attempts", 0}}},
        {"endpoint", {{"base_url", "http://127.0.0.1:" + std::to_string(port)}}},
    };
}

/// One user turn carrying @p text — the marker the server echoes back.
model_io::AgentInputState one_turn(const std::string& text) {
    model_io::AgentInputState state;
    model_io::MessageItem user;
    user.type = model_io::MessageItemType::UserInput;
    user.role = "user";
    model_io::Content content;
    content.type = model_io::ContentType::Text;
    content.raw = text;
    user.content = {content};
    state.turns.emplace_back().user_input = user;
    return state;
}

/// The marker extractor: the user text of the request body's last message.
std::string marker_of_request(const std::string& body) {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded() || !json.contains("messages")) return "?";
    const auto& messages = json["messages"];
    if (!messages.is_array() || messages.empty()) return "?";
    const auto& content = messages.back()["content"];
    return content.is_string() ? content.get<std::string>() : "?";
}

} // namespace

// ---------------------------------------------------------------------------
// Concurrent exchanges on one instance do not cross-contaminate
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(concurrent_converse_keeps_each_exchange_separate) {
    constexpr std::size_t kExchanges = 4;
    ConcurrentServer server(kExchanges, marker_of_request);
    const auto port = server.wait_listening();

    asio::io_context io;
    FixtureModel model(io.get_executor(), loopback_config(port));
    BOOST_REQUIRE(model.build());

    // Events from every exchange land in ONE slot, from several threads — the
    // subscriber contract concurrency imposes. Demultiplexed by exchange_id.
    std::mutex bus_mutex;
    std::map<std::string, std::string> reasoning_by_exchange;
    // ScopedSubscription, NOT the bare Connection subscribe() returns: the bus
    // is process-wide, so a slot left registered would keep capturing these
    // stack locals after the test returned.
    eventbus::EventBus::ScopedSubscription view = eventbus::default_bus()
        .subscribe<llm::chat_completions::ReasoningDeltaEvent>(
            [&](const llm::chat_completions::ReasoningDeltaEvent& e) {
                std::lock_guard<std::mutex> lock(bus_mutex);
                reasoning_by_exchange[e.exchange_id] += e.reasoning;
            });

    std::vector<std::optional<model_io::MessageItem>> results(kExchanges);
    std::vector<std::exception_ptr> failures(kExchanges);
    for (std::size_t index = 0; index < kExchanges; ++index) {
        asio::co_spawn(io, [&, index]() -> asio::awaitable<void> {
            try {
                results[index] = co_await model.converse(
                    one_turn("req-" + std::to_string(index)));
            } catch (...) {
                failures[index] = std::current_exception();
            }
        }, asio::detached);
    }

    // Four threads: the exchanges genuinely run in parallel, so the events are
    // published from different threads into the one slot above.
    std::vector<std::thread> pool;
    for (int worker = 0; worker < 4; ++worker) {
        pool.emplace_back([&io] { io.run(); });
    }
    for (auto& worker : pool) worker.join();
    server.join();

    for (std::size_t index = 0; index < kExchanges; ++index) {
        if (failures[index]) std::rethrow_exception(failures[index]);
        BOOST_REQUIRE_MESSAGE(results[index], "exchange " << index << " produced no result");
    }

    // Each exchange got the answer to ITS OWN request — the pairing, not just
    // the count, is what rules out cross-contamination.
    std::set<std::string> ids;
    for (std::size_t index = 0; index < kExchanges; ++index) {
        const std::string marker = "req-" + std::to_string(index);
        const auto& item = *results[index];
        BOOST_REQUIRE_EQUAL(item.content.size(), 1u);
        BOOST_CHECK_EQUAL(item.content[0].raw, "answer-" + marker);
        BOOST_REQUIRE(item.reasoning);
        BOOST_CHECK_EQUAL(item.reasoning->raw, "think-" + marker);

        // Every exchange reports its correlation id, and they are all distinct.
        BOOST_REQUIRE(item.extras);
        BOOST_REQUIRE(item.extras->contains("exchange_id"));
        const auto id = (*item.extras)["exchange_id"].get<std::string>();
        BOOST_CHECK(!id.empty());
        BOOST_CHECK_MESSAGE(ids.insert(id).second,
                            "exchange_id " << id << " was reused across exchanges");

        // The live stream is bindable to this result through that id, and
        // carries only this exchange's increments.
        std::lock_guard<std::mutex> lock(bus_mutex);
        BOOST_REQUIRE_MESSAGE(reasoning_by_exchange.count(id) == 1,
                              "no broadcast for exchange_id " << id);
        BOOST_CHECK_EQUAL(reasoning_by_exchange[id], "think-" + marker);
    }
    BOOST_CHECK_EQUAL(ids.size(), kExchanges);

    // All four requests reached the wire, each exactly once.
    const auto captured = server.captured();
    BOOST_REQUIRE_EQUAL(captured.size(), kExchanges);
    std::set<std::string> wire_markers;
    for (const auto& body : captured) wire_markers.insert(marker_of_request(body));
    BOOST_CHECK_EQUAL(wire_markers.size(), kExchanges);
}

// ---------------------------------------------------------------------------
// set_generation() concurrent with in-flight exchanges cannot tear a snapshot
// ---------------------------------------------------------------------------
// The race this pins used to be real: converse() read _generation directly, so
// a host re-tuning knobs mid-flight raced every read. Each exchange now takes
// one snapshot before its first suspension, so a request body must carry a
// COHERENT pair of knobs — never temperature from before the patch beside
// top_p from after it.
BOOST_AUTO_TEST_CASE(concurrent_set_generation_never_tears_an_exchange) {
    constexpr std::size_t kExchanges = 4;
    ConcurrentServer server(kExchanges, marker_of_request);
    const auto port = server.wait_listening();

    asio::io_context io;
    FixtureModel model(io.get_executor(), loopback_config(port));
    BOOST_REQUIRE(model.build());
    // The baseline pair the patches move together.
    model.set_generation(nlohmann::json{{"temperature", 0.1}, {"top_p", 0.1}});

    std::vector<std::optional<model_io::MessageItem>> results(kExchanges);
    std::vector<std::exception_ptr> failures(kExchanges);
    for (std::size_t index = 0; index < kExchanges; ++index) {
        asio::co_spawn(io, [&, index]() -> asio::awaitable<void> {
            try {
                results[index] = co_await model.converse(
                    one_turn("req-" + std::to_string(index)));
            } catch (...) {
                failures[index] = std::current_exception();
            }
        }, asio::detached);
    }

    // A writer hammering set_generation() from OUTSIDE the executor threads,
    // for the whole life of the exchanges: patches always move both knobs to
    // the same value, so any mixed pair on the wire is a torn read.
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (double value = 0.2; !stop.load(std::memory_order_relaxed);
             value += 0.1) {
            const double knob = 0.1 + std::fmod(value, 0.8);
            model.set_generation(nlohmann::json{
                {"temperature", knob}, {"top_p", knob}});
            std::this_thread::yield();
        }
    });

    std::vector<std::thread> pool;
    for (int worker = 0; worker < 4; ++worker) {
        pool.emplace_back([&io] { io.run(); });
    }
    for (auto& worker : pool) worker.join();
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    server.join();

    for (std::size_t index = 0; index < kExchanges; ++index) {
        if (failures[index]) std::rethrow_exception(failures[index]);
        BOOST_REQUIRE(results[index]);
    }

    // Every body carries one coherent snapshot, and a live "model" key (the
    // invariant apply_generation_patch enforces) throughout.
    const auto captured = server.captured();
    BOOST_REQUIRE_EQUAL(captured.size(), kExchanges);
    for (const auto& body : captured) {
        const auto request = nlohmann::json::parse(body);
        BOOST_REQUIRE(request.contains("temperature"));
        BOOST_REQUIRE(request.contains("top_p"));
        BOOST_CHECK_EQUAL(request["temperature"].get<double>(),
                          request["top_p"].get<double>());
        BOOST_CHECK_EQUAL(request["model"], "fixture-model");
    }
    // The knobs survived the hammering as a valid object.
    const auto final_knobs = model.generation();
    BOOST_CHECK_EQUAL(final_knobs["temperature"].get<double>(),
                      final_knobs["top_p"].get<double>());
}

// ---------------------------------------------------------------------------
// The by-value AgentInputState: a temporary argument is safe
// ---------------------------------------------------------------------------
// With the former `const AgentInputState&` parameter this was a dangling read:
// the awaitable is created here but first resumed inside co_spawn, by which
// point the temporary is long gone. By value the coroutine frame owns its copy.
BOOST_AUTO_TEST_CASE(converse_owns_its_conversation_argument) {
    ConcurrentServer server(1, marker_of_request);
    const auto port = server.wait_listening();

    asio::io_context io;
    FixtureModel model(io.get_executor(), loopback_config(port));
    BOOST_REQUIRE(model.build());

    std::optional<model_io::MessageItem> result;
    std::exception_ptr failure;
    // The trap, spelled out: the argument is a temporary, and the awaitable is
    // NOT awaited in the same full-expression that created it.
    auto exchange = model.converse(one_turn("req-temporary"));
    asio::co_spawn(io, [&, pending = std::move(exchange)]() mutable
                       -> asio::awaitable<void> {
        try {
            result = co_await std::move(pending);
        } catch (...) {
            failure = std::current_exception();
        }
    }, asio::detached);
    io.run();
    server.join();

    if (failure) std::rethrow_exception(failure);
    BOOST_REQUIRE(result);
    BOOST_REQUIRE_EQUAL(result->content.size(), 1u);
    BOOST_CHECK_EQUAL(result->content[0].raw, "answer-req-temporary");
    // The conversation survived intact all the way to the wire.
    const auto captured = server.captured();
    BOOST_REQUIRE_EQUAL(captured.size(), 1u);
    BOOST_CHECK_EQUAL(marker_of_request(captured[0]), "req-temporary");
}

// ---------------------------------------------------------------------------
// A retried exchange keeps its id and advances the attempt counter
// ---------------------------------------------------------------------------
// The replay ambiguity this resolves: a retry re-broadcasts the SAME
// increments under the SAME exchange_id, so a subscriber that only appends
// would double the prefix. `attempt` advancing is the signal to start over.
BOOST_AUTO_TEST_CASE(retried_exchange_replays_under_one_id_with_attempt_advanced) {
    // Attempt 0 is truncated (server closes after the reasoning increment,
    // never sending the terminal event) — recoverable, so complete() re-reads
    // from scratch. Attempt 1 is the whole stream.
    const std::string truncated =
        sse(nlohmann::json{
            {"id", "chatcmpl-retry"},
            {"object", "chat.completion.chunk"},
            {"model", "fixture-model"},
            {"choices", nlohmann::json::array({{
                {"index", 0},
                {"delta", {{"role", "assistant"},
                           {"reasoning_content", "think-retry"}}},
                {"finish_reason", nullptr},
            }})},
        });

    loopback::SequenceServer server({
        [&](asio::ip::tcp::socket& socket) {
            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            http::read(socket, buffer, request);
            http::response<http::string_body> response(http::status::ok, 11);
            response.set(http::field::content_type, "text/event-stream");
            response.set(http::field::connection, "close");
            response.body() = truncated;
            response.prepare_payload();
            http::write(socket, response);
        },
        [&](asio::ip::tcp::socket& socket) {
            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            http::read(socket, buffer, request);
            http::response<http::string_body> response(http::status::ok, 11);
            response.set(http::field::content_type, "text/event-stream");
            response.set(http::field::connection, "close");
            response.body() = scripted_stream("retry");
            response.prepare_payload();
            http::write(socket, response);
        },
    });
    const auto port = server.wait_listening();

    asio::io_context io;
    auto config = loopback_config(port);
    config["retry"] = nlohmann::json{{"max_attempts", 1},
                                     {"initial_backoff_ms", 1}};
    FixtureModel model(io.get_executor(), config);
    BOOST_REQUIRE(model.build());

    std::vector<llm::chat_completions::ReasoningDeltaEvent> broadcast;
    // ScopedSubscription: see the note in the concurrent case above. This
    // exchange is single-threaded, so the vector needs no lock.
    eventbus::EventBus::ScopedSubscription view = eventbus::default_bus()
        .subscribe<llm::chat_completions::ReasoningDeltaEvent>(
            [&](const llm::chat_completions::ReasoningDeltaEvent& e) {
                broadcast.push_back(e);
            });

    std::optional<model_io::MessageItem> result;
    std::exception_ptr failure;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            result = co_await model.converse(one_turn("req-retry"));
        } catch (...) {
            failure = std::current_exception();
        }
    }, asio::detached);
    io.run();
    server.join();

    if (failure) std::rethrow_exception(failure);
    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(result->content[0].raw, "answer-retry");

    // Both attempts broadcast the same reasoning text under ONE id, and the
    // attempt counter is what tells the replay from the original.
    BOOST_REQUIRE_EQUAL(broadcast.size(), 2u);
    BOOST_CHECK_EQUAL(broadcast[0].exchange_id, broadcast[1].exchange_id);
    BOOST_CHECK_EQUAL(broadcast[0].reasoning, "think-retry");
    BOOST_CHECK_EQUAL(broadcast[1].reasoning, "think-retry");
    BOOST_CHECK_EQUAL(broadcast[0].attempt, 0u);
    BOOST_CHECK_EQUAL(broadcast[1].attempt, 1u);
    // The result reports that same id, so the retried stream still binds to it.
    BOOST_REQUIRE(result->extras);
    BOOST_CHECK_EQUAL((*result->extras)["exchange_id"],
                      broadcast[0].exchange_id);
    // Naively appending would have produced the doubled prefix the attempt
    // counter exists to prevent.
    BOOST_CHECK_EQUAL(result->reasoning->raw, "think-retry");
}
