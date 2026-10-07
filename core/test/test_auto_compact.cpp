#define BOOST_TEST_MODULE CoreAutoCompact
#include <boost/test/unit_test.hpp>
#include "core/application.hpp"
#include "core/protocol.hpp"
#include "load/persistence.hpp"
#include <boost/asio/use_future.hpp>
#include <boost/beast.hpp>
#include <thread>

namespace asio = boost::asio;
namespace beast = boost::beast;
using Json = nlohmann::json;
using State = model_io::AgentInputState;

namespace {
enum class Mode { Success, Frequent, Budget, LastBudgetSuccess, SummaryFailure, SummaryEmpty, SummaryTools, Cancel, CancelResume, MemoryContinue };

/** No credentials or shell effects: unknown fixture calls settle through the registry. */
struct Model : llm::LLMModel {
    Model(asio::any_io_executor executor, Mode mode) : LLMModel(executor, {}), mode(mode) {}
    llm::LLMModelType model_type() const noexcept override { return llm::LLMModelType::Conversation; }
    Mode mode;
    int ordinary = 0;
    int summaries = 0;
    bool waiting = false;
    std::vector<State> contexts;
    asio::awaitable<model_io::MessageItem> converse(State state) override {
        contexts.push_back(state);
        const auto prompt = state.turns.back().user_input.content.front().raw;
        const bool summary = prompt == "HANDOFF";
        model_io::MessageItem result;
        result.type = model_io::MessageItemType::ModelResponse;
        result.role = "assistant";
        model_io::Content content;
        if (summary) {
            ++summaries;
            BOOST_TEST(state.tools.empty());
            if (mode == Mode::SummaryFailure) throw std::runtime_error("summary provider unavailable");
            if (mode == Mode::Cancel) {
                waiting = true;
                asio::steady_timer wait(co_await asio::this_coro::executor, std::chrono::seconds(20));
                co_await wait.async_wait(asio::use_awaitable);
            }
            content.raw = mode == Mode::SummaryEmpty ? " \n"
                : "Goal: finish original task. State: verified work. Retrieve: /workspace/results. Next: verify.";
            if (mode == Mode::SummaryTools) {
                model_io::InvokeQuery forbidden;
                forbidden.id = "forbidden";
                forbidden.name = "unregistered_fixture";
                result.invokes = std::vector<model_io::InvokeQuery>{forbidden};
            }
            result.cost = model_io::TokenCost{500, 10, 100};
        } else {
            ++ordinary;
            if (mode == Mode::CancelResume && ordinary == 3) {
                waiting = true;
                asio::steady_timer wait(co_await asio::this_coro::executor, std::chrono::seconds(20));
                co_await wait.async_wait(asio::use_awaitable);
            }
            if (ordinary > 2 || mode == Mode::MemoryContinue) {
                BOOST_TEST(prompt == "PRIVATE RESUME");
                BOOST_REQUIRE(state.turns.back().user_input.extras);
                BOOST_TEST(state.turns.back().user_input.extras->at("simplex.internal_input") == "auto_compact_continue");
            }
            const bool final = mode == Mode::MemoryContinue
                || ((mode == Mode::Success || mode == Mode::LastBudgetSuccess) && ordinary == 5);
            content.raw = final ? "Final answer" : std::string(20000, 'X');
            // Prompt cost deliberately exceeds the threshold even on final answers.
            // Every second exchange triggers; summary usage never triggers recursion.
            result.cost = model_io::TokenCost{ordinary % 2 == 0 || final ? 200u : 50u, 10, 0};
            if (!final) {
                model_io::InvokeQuery query;
                query.id = "fixture-" + std::to_string(ordinary);
                query.name = "unregistered_fixture";
                result.invokes = std::vector<model_io::InvokeQuery>{query};
            }
        }
        result.content.push_back(std::move(content));
        co_return result;
    }
};

void exercise(Mode mode, bool save_run) {
    const auto root = std::filesystem::temp_directory_path() / ("simplex-auto-" + core::new_identity());
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    load::Configuration config;
    config.directory = root;
    config.document = Json::object();
    config.provider = "fixture";
    config.client = load::websocket_endpoint("ws://127.0.0.1:"
        + std::to_string(acceptor.local_endpoint().port()) + "/events");
    config.storage = root;
    config.state_directory = root / "state";
    config.memory = root / "memory";
    config.compact_prompt = "MANUAL";
    config.auto_compact_prompt = "HANDOFF";
    config.auto_compact_continue_prompt = "PRIVATE RESUME";
    config.auto_compact_threshold = 100;
    config.max_exchanges = mode == Mode::Frequent ? 1 : 10;
    config.max_auto_compactions = 2;
    config.save_run = save_run;
    config.save_step = config.save_shutdown = false;
    auto model = std::make_shared<Model>(io.get_executor(), mode);
    if (mode == Mode::MemoryContinue) {
        State state;
        state.meta.session_id = "test";
        state.loop.emplace();
        state.system_prompt.add_section("memory.runtime", "Memory", "Goal: finish original task.",
            model_io::SectionStability::Volatile);
        load::save_state(config.state_directory / "state.json", state);
    }
    core::Application app(io.get_executor(), config, "test", model);
    std::vector<Json> events;
    auto peer = [&]() -> asio::awaitable<void> {
        beast::websocket::stream<asio::ip::tcp::socket> socket(
            co_await acceptor.async_accept(asio::use_awaitable));
        co_await socket.async_accept(asio::use_awaitable);
        auto send = [&](std::string type, Json data) -> asio::awaitable<void> {
            const auto wire = Json{{"type", type}, {"data", std::move(data)}}.dump();
            co_await socket.async_write(asio::buffer(wire), asio::use_awaitable);
        };
        for (;;) {
            beast::flat_buffer buffer;
            boost::system::error_code error;
            co_await socket.async_read(buffer, asio::redirect_error(asio::use_awaitable, error));
            if (error) break;
            const auto event = Json::parse(beast::buffers_to_string(buffer.data()));
            events.push_back(event);
            const auto name = event.at("event").get<std::string>();
            if (name == "ready") {
                Json input = {{"operation", "continue"}, {"request_id", "original"}};
                if (mode != Mode::MemoryContinue) {
                    input["operation"] = "message";
                    input["content"] = Json::array({{{"type", "text"}, {"modality", "text"},
                        {"raw", "Original task"}}});
                }
                co_await send("payload", std::move(input));
            } else if ((name == "tool_calls" && mode == Mode::Cancel
                && event.at("data").at(0).at("name") == "auto_compact")
                || (name == "compact_finished" && mode == Mode::CancelResume)) {
                while (!model->waiting) {
                    asio::steady_timer wait(co_await asio::this_coro::executor, std::chrono::milliseconds(1));
                    co_await wait.async_wait(asio::use_awaitable);
                }
                co_await send("signal", {{"operation", "cancel"}, {"run_id", event.at("run_id")}});
            } else if (name == "run_finished") {
                co_await send("payload", {{"operation", "history"}, {"request_id", "inspect"}});
            } else if (name == "history") {
                BOOST_TEST(event.dump().find("PRIVATE RESUME") == std::string::npos);
                if (mode == Mode::Success || mode == Mode::LastBudgetSuccess || mode == Mode::MemoryContinue) {
                    BOOST_TEST(event.at("data").at("turns").at(0).at("user").empty());
                    BOOST_TEST(event.at("data").at("turns").at(0).at("internal_input") == "auto_compact_continue");
                    BOOST_TEST(event.at("data").at("turns").at(0).at("source").at("request_id") == "original");
                }
                co_await send("signal", {{"operation", "shutdown"}});
            } else if (name == "input_rejected" || name == "error") {
                BOOST_FAIL(event.dump());
            }
        }
    };
    auto peer_done = asio::co_spawn(io, peer(), asio::use_future);
    auto worker_done = asio::co_spawn(io, app.run(), asio::use_future);
    asio::steady_timer watchdog(io, std::chrono::seconds(10));
    bool timed_out = false;
    watchdog.async_wait([&](auto error) { if (!error) { timed_out = true; app.stop(); } });
    // Cancel the watchdog once the peer has drained; no timing sleeps in assertions.
    std::jthread runner([&] { io.run(); });
    worker_done.get();
    peer_done.get();
    asio::post(io, [&] { watchdog.cancel(); });
    runner.join();
    BOOST_TEST(!timed_out);
    int starts = 0, finishes = 0, inputs = 0, calls = 0, results = 0;
    Json finished;
    std::string run;
    for (const auto& event : events) {
        const auto name = event.at("event");
        if (name == "input_admitted") run = event.at("run_id").get<std::string>();
        if (name == "run_started") ++starts;
        if (name == "input_committed") ++inputs;
        if (name == "run_finished") { ++finishes; finished = event.at("data"); }
        if (name == "tool_calls" && event.at("data").at(0).at("name") == "auto_compact") {
            ++calls;
            BOOST_TEST(event.at("data").at(0).at("security") == "trusted");
            BOOST_TEST(event.at("data").at(0).at("type") == "serial_write");
        }
        if (name == "tool_results"
            && event.at("data").at(0).at("invoke_return").at("query").at("name") == "auto_compact") ++results;
        if (name == "compact_finished") BOOST_TEST(event.at("data").at("origin") == "automatic");
        if (name != "ready" && name != "history") BOOST_TEST(event.at("run_id") == run);
    }
    BOOST_TEST(starts == 1);
    BOOST_TEST(finishes == 1);
    BOOST_TEST(inputs == (mode == Mode::MemoryContinue ? 0 : 1));
    BOOST_TEST(calls == model->summaries);
    BOOST_TEST(results == calls);
    BOOST_TEST(finished.at("task_exchanges") == model->ordinary - (mode == Mode::CancelResume ? 1 : 0));
    BOOST_TEST(finished.at("compact_exchanges") ==
        ((mode == Mode::SummaryFailure || mode == Mode::Cancel) ? 0 : model->summaries));
    if (mode == Mode::Success || mode == Mode::LastBudgetSuccess || mode == Mode::MemoryContinue) {
        BOOST_TEST(finished.at("status") == "completed");
        BOOST_TEST(model->summaries == (mode == Mode::MemoryContinue ? 0 : 2));
    } else if (mode == Mode::Cancel || mode == Mode::CancelResume) {
        BOOST_TEST(finished.at("status") == "cancelled");
    } else {
        BOOST_TEST(finished.at("status") == "failed");
        const std::string expected = mode == Mode::Frequent ? "auto_compact_too_frequent"
            : mode == Mode::Budget ? "auto_compact_limit"
            : mode == Mode::SummaryEmpty ? "empty text summary"
            : mode == Mode::SummaryTools ? "must not call tools" : "summary provider unavailable";
        BOOST_TEST(finished.at("error").get<std::string>().find(expected) != std::string::npos);
        BOOST_TEST(finished.at("failure").at("operation") == "auto_compact");
    }
    if (save_run || mode == Mode::Cancel) {
        const auto state = load::load_state(config.state_directory / "state.json");
        BOOST_CHECK(state.loop->phase == model_io::LoopPhase::Ready);
        BOOST_TEST(finished.at("durable") == true);
        if (mode == Mode::Success) {
            BOOST_TEST(state.loop->committed_response_sequence == 7);
            BOOST_TEST(model_io::external_status(state, "context_statistic").value()
                .at("sampled_exchange_count") == 7);
        }
    } else {
        // A successful intermediate compact save is not the final continuation save.
        BOOST_TEST(finished.at("durable") == false);
    }
}
}
BOOST_AUTO_TEST_CASE(multiple_cycles_preserve_identity_and_usage) { exercise(Mode::Success, true); }
BOOST_AUTO_TEST_CASE(intermediate_save_does_not_claim_final_durability) { exercise(Mode::Success, false); }
BOOST_AUTO_TEST_CASE(immediate_retrigger_stops_before_second_archive) { exercise(Mode::Frequent, true); }
BOOST_AUTO_TEST_CASE(finite_budget_stops_repeated_two_exchange_cycles) { exercise(Mode::Budget, true); }
BOOST_AUTO_TEST_CASE(last_budget_allows_final_answer) { exercise(Mode::LastBudgetSuccess, true); }
BOOST_AUTO_TEST_CASE(summary_failure_settles_synthetic_tool) { exercise(Mode::SummaryFailure, true); }
BOOST_AUTO_TEST_CASE(cancel_interrupts_summary_without_continuation) { exercise(Mode::Cancel, true); }
BOOST_AUTO_TEST_CASE(explicit_continue_from_memory_is_private) { exercise(Mode::MemoryContinue, true); }

BOOST_AUTO_TEST_CASE(cancel_after_replacement_keeps_memory_and_settled_compact_card) { exercise(Mode::CancelResume, true); }
BOOST_AUTO_TEST_CASE(invalid_summary_keeps_original_state_and_stops) { exercise(Mode::SummaryEmpty, true); }
BOOST_AUTO_TEST_CASE(summary_cannot_call_tools_or_recurse) { exercise(Mode::SummaryTools, true); }
