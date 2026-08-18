// Deterministic, offline tests for the Responses-API stream READER
// (ResponsesReader over ModelResponseReader): the consume loop next(), the
// delta accumulation into the assembled model_io::MessageItem, StreamStatus
// (terminal and Aborted), the sync/async hooks that replaced PeekingHandler,
// and the centralized exception policy. A plain boost::asio::io_context
// drives the put/next coroutines against the reader's own handler (no
// sockets, no TLS): every test runs its io_context to quiescence, so each
// stream fed here ends — via its terminal event, a fault, or abort().
#define BOOST_TEST_MODULE responses_reader
#include <boost/test/unit_test.hpp>

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "endpoint/http_request_exception.hpp"
#include "endpoint/https_stream.hpp"
#include "endpoint/request.hpp"
#include "endpoint/responses/delta.hpp"
#include "endpoint/responses/reader.hpp"
#include "loopback_server.hpp"

namespace asio = boost::asio;
namespace http = boost::beast::http;

using endpoint::responses::DeltaKind;
using endpoint::responses::ResponsesDelta;
using endpoint::responses::ResponsesReader;
using endpoint::responses::StreamStatus;
using endpoint::responses::is_terminal;

// --- helpers ------------------------------------------------------------------

// One SSE frame carrying the given event JSON.
static std::string sse(const nlohmann::json& event) {
    return "event: " + event.at("type").get<std::string>() + "\ndata: " +
           event.dump() + "\n\n";
}

// The canonical happy-path text stream: 10 frames, 10 deltas, terminal
// response.completed (so the reader assembles and finishes cleanly).
static std::string text_stream() {
    nlohmann::json done_part =
        nlohmann::json{{"type", "output_text"}, {"text", "The answer is 42"},
                       {"annotations", nlohmann::json::array()}};
    const std::string text = "The answer is 42";
    nlohmann::json done_item =
        nlohmann::json{{"id", "msg_1"},
                       {"type", "message"},
                       {"role", "assistant"},
                       {"status", "completed"},
                       {"content", nlohmann::json::array({done_part})}};
    return sse(nlohmann::json{{"type", "response.created"},
                              {"response", {{"id", "resp_1"}}}}) +
           sse(nlohmann::json{{"type", "response.output_item.added"},
                              {"output_index", 0},
                              {"item", {{"id", "msg_1"},
                                        {"type", "message"},
                                        {"role", "assistant"},
                                        {"content", nlohmann::json::array()}}}}) +
           sse(nlohmann::json{{"type", "response.content_part.added"},
                              {"item_id", "msg_1"},
                              {"output_index", 0},
                              {"content_index", 0},
                              {"part", {{"type", "output_text"}, {"text", ""}}}}) +
           sse(nlohmann::json{{"type", "response.output_text.delta"},
                              {"item_id", "msg_1"},
                              {"output_index", 0},
                              {"content_index", 0},
                              {"delta", "The"}}) +
           sse(nlohmann::json{{"type", "response.output_text.delta"},
                              {"item_id", "msg_1"},
                              {"output_index", 0},
                              {"content_index", 0},
                              {"delta", " answer"}}) +
           sse(nlohmann::json{{"type", "response.output_text.delta"},
                              {"item_id", "msg_1"},
                              {"output_index", 0},
                              {"content_index", 0},
                              {"delta", " is 42"}}) +
           sse(nlohmann::json{{"type", "response.output_text.done"},
                              {"item_id", "msg_1"},
                              {"output_index", 0},
                              {"content_index", 0},
                              {"text", text}}) +
           sse(nlohmann::json{{"type", "response.content_part.done"},
                              {"item_id", "msg_1"},
                              {"output_index", 0},
                              {"content_index", 0},
                              {"part", done_part}}) +
           sse(nlohmann::json{{"type", "response.output_item.done"},
                              {"output_index", 0},
                              {"item", done_item}}) +
           sse(nlohmann::json{{"type", "response.completed"},
                              {"response",
                               {{"id", "resp_1"},
                                {"usage", {{"input_tokens", 10}, {"output_tokens", 5}}}}}});
}

// Drive one reader over `chunk` to the stream's end: spawn the canonical
// consume loop plus the producer put on the reader's own handler, and run
// the io_context to quiescence (every caller's chunk ends the stream).
// Returns the deltas next() delivered — the terminal marker INCLUDED.
// Rethrows anything the loop surfaced (the throwing-hook path).
static std::vector<ResponsesDelta> read_all(
    asio::io_context& io, ResponsesReader& reader, std::string_view chunk) {
    std::vector<ResponsesDelta> out;
    std::optional<std::exception_ptr> error;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                while (auto delta = co_await reader.next()) {
                    out.push_back(std::move(*delta));
                }
            } catch (...) {
                error = std::current_exception();
            }
        },
        asio::detached);
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                co_await reader.handler()->put(chunk);
            } catch (const endpoint::SSEAborted&) {
                // Cooperative stop: the reader closed the channel (a hook
                // fault) while this chunk still had events to deliver.
            }
        },
        asio::detached);
    io.run();
    io.restart();
    if (error) std::rethrow_exception(*error);
    return out;
}

// --- the consume loop ---------------------------------------------------------

BOOST_AUTO_TEST_CASE(next_returns_every_delta_then_stops_idempotently) {
    asio::io_context io;
    ResponsesReader reader(io.get_executor());

    auto deltas = read_all(io, reader, text_stream());
    BOOST_REQUIRE_EQUAL(deltas.size(), 10u);   // includes the terminal marker

    BOOST_CHECK(reader.finished());
    BOOST_CHECK(reader.status() == StreamStatus::Completed);

    // next() after the end returns nullopt, forever, without touching the
    // (long-closed) channel.
    std::optional<ResponsesDelta> again;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            again = co_await reader.next();
            again = co_await reader.next();
        },
        asio::detached);
    io.run();
    BOOST_CHECK(!again.has_value());
}

// --- accumulation / assembly ---------------------------------------------------

BOOST_AUTO_TEST_CASE(text_stream_assembles_the_contract_record) {
    asio::io_context io;
    ResponsesReader reader(io.get_executor());
    read_all(io, reader, text_stream());

    const auto& response = reader.response();
    BOOST_CHECK(response.type == model_io::MessageItemType::ModelResponse);
    BOOST_CHECK_EQUAL(response.role, "assistant");
    BOOST_CHECK_EQUAL(response.content.raw, "The answer is 42");
    BOOST_CHECK(!response.reasoning);
    BOOST_CHECK(!response.invokes);
    BOOST_REQUIRE(response.extras);
    BOOST_CHECK_EQUAL((*response.extras)["response_id"], "resp_1");
    BOOST_CHECK_EQUAL((*response.extras)["usage"]["input_tokens"], 10);
    BOOST_REQUIRE((*response.extras)["output_items"].is_array());
    BOOST_REQUIRE_EQUAL((*response.extras)["output_items"].size(), 1u);
    BOOST_CHECK_EQUAL((*response.extras)["output_items"][0]["id"], "msg_1");
}

BOOST_AUTO_TEST_CASE(function_call_stream_assembles_invokes) {
    asio::io_context io;
    ResponsesReader reader(io.get_executor());

    const std::string chunk =
        sse(nlohmann::json{{"type", "response.created"},
                           {"response", {{"id", "resp_2"}}}}) +
        sse(nlohmann::json{{"type", "response.output_item.added"},
                           {"output_index", 0},
                           {"item", {{"id", "rs_1"},
                                     {"type", "reasoning"},
                                     {"summary", nlohmann::json::array()}}}}) +
        sse(nlohmann::json{{"type", "response.reasoning_text.delta"},
                           {"item_id", "rs_1"},
                           {"delta", "Hmm"}}) +
        sse(nlohmann::json{{"type", "response.reasoning_text.delta"},
                           {"item_id", "rs_1"},
                           {"delta", " ..."}}) +
        sse(nlohmann::json{
            {"type", "response.reasoning_text.done"},
            {"item_id", "rs_1"},
            {"text", "Hmm ... let me check the weather."}}) +
        sse(nlohmann::json{{"type", "response.output_item.done"},
                           {"output_index", 0},
                           {"item", {{"id", "rs_1"},
                                     {"type", "reasoning"},
                                     {"summary", nlohmann::json::array()},
                                     {"encrypted_content", "ENC"}}}}) +
        sse(nlohmann::json{
            {"type", "response.output_item.added"},
            {"output_index", 1},
            {"item", {{"id", "fc_1"},
                      {"type", "function_call"},
                      {"call_id", "call_1"},
                      {"name", "get_weather"},
                      {"arguments", ""}}}}) +
        sse(nlohmann::json{{"type", "response.function_call_arguments.delta"},
                           {"item_id", "fc_1"},
                           {"delta", "{\"city\":"}}) +
        sse(nlohmann::json{{"type", "response.function_call_arguments.delta"},
                           {"item_id", "fc_1"},
                           {"delta", "\"London\"}"}}) +
        sse(nlohmann::json{{"type", "response.function_call_arguments.done"},
                           {"item_id", "fc_1"},
                           {"name", "get_weather"},
                           {"arguments", "{\"city\":\"London\"}"}}) +
        sse(nlohmann::json{{"type", "response.output_item.done"},
                           {"output_index", 1},
                           {"item", {{"id", "fc_1"},
                                     {"type", "function_call"},
                                     {"call_id", "call_1"},
                                     {"name", "get_weather"},
                                     {"arguments", "{\"city\":\"London\"}"},
                                     {"status", "completed"}}}}) +
        sse(nlohmann::json{{"type", "response.completed"},
                           {"response", {{"id", "resp_2"}}}});
    read_all(io, reader, chunk);

    BOOST_CHECK(reader.status() == StreamStatus::Completed);
    const auto& response = reader.response();
    // Reasoning: authoritative done item captured for round-tripping.
    BOOST_REQUIRE(response.reasoning);
    BOOST_CHECK_EQUAL(response.reasoning->raw, "Hmm ... let me check the weather.");
    BOOST_REQUIRE(response.reasoning->extras);
    BOOST_REQUIRE((*response.reasoning->extras)["items"].is_array());
    BOOST_REQUIRE_EQUAL((*response.reasoning->extras)["items"].size(), 1u);
    BOOST_CHECK_EQUAL((*response.reasoning->extras)["items"][0]["encrypted_content"],
                      "ENC");
    // The call: id is the correlation call_id, arguments parsed to JSON.
    BOOST_REQUIRE(response.invokes);
    BOOST_REQUIRE_EQUAL(response.invokes->size(), 1u);
    BOOST_CHECK_EQUAL(response.invokes->at(0).id, "call_1");
    BOOST_CHECK_EQUAL(response.invokes->at(0).name, "get_weather");
    const nlohmann::json expected_arguments = {{"city", "London"}};
    BOOST_CHECK_EQUAL(response.invokes->at(0).arguments, expected_arguments);
    BOOST_REQUIRE(response.invokes->at(0).extras);
    BOOST_CHECK_EQUAL((*response.invokes->at(0).extras)["id"], "fc_1");
    // Text-less response: content is empty but the record is well-formed.
    BOOST_CHECK_EQUAL(response.content.raw, "");
}

BOOST_AUTO_TEST_CASE(malformed_call_arguments_fall_back_to_the_raw_string) {
    asio::io_context io;
    ResponsesReader reader(io.get_executor());

    const std::string chunk =
        sse(nlohmann::json{{"type", "response.output_item.added"},
                           {"output_index", 0},
                           {"item", {{"id", "fc_9"},
                                     {"type", "function_call"},
                                     {"call_id", "call_9"},
                                     {"name", "tool"},
                                     {"arguments", ""}}}}) +
        sse(nlohmann::json{{"type", "response.function_call_arguments.done"},
                           {"item_id", "fc_9"},
                           {"arguments", "{not json"}}) +
        sse(nlohmann::json{{"type", "response.completed"},
                           {"response", {{"id", "r"}}}});
    read_all(io, reader, chunk);

    const auto& invokes = *reader.response().invokes;
    BOOST_REQUIRE_EQUAL(invokes.size(), 1u);
    BOOST_CHECK_EQUAL(invokes[0].arguments, "{not json");   // kept verbatim
}

BOOST_AUTO_TEST_CASE(reasoning_summary_and_text_are_distinct_channels) {
    asio::io_context io;
    ResponsesReader summary_only(io.get_executor());
    const std::string chunk =
        sse(nlohmann::json{{"type", "response.output_item.added"},
                           {"output_index", 0},
                           {"item", {{"id", "rs_s"},
                                     {"type", "reasoning"},
                                     {"summary", nlohmann::json::array()}}}}) +
        sse(nlohmann::json{{"type", "response.reasoning_summary_part.added"},
                           {"item_id", "rs_s"},
                           {"summary_index", 0}}) +
        sse(nlohmann::json{{"type", "response.reasoning_summary_text.delta"},
                           {"item_id", "rs_s"},
                           {"summary_index", 0},
                           {"delta", "Short version."}}) +
        sse(nlohmann::json{{"type", "response.completed"},
                           {"response", {{"id", "r"}}}});
    auto deltas = read_all(io, summary_only, chunk);
    BOOST_REQUIRE_EQUAL(deltas.size(), 4u);
    BOOST_CHECK(deltas[2].kind == DeltaKind::ReasoningSummary);
    const auto& response = summary_only.response();
    BOOST_REQUIRE(response.reasoning);
    BOOST_CHECK_EQUAL(response.reasoning->raw, "Short version.");   // fallback

    // With both channels present the raw thinking wins over the summary.
    asio::io_context io2;
    ResponsesReader both(io2.get_executor());
    const std::string chunk2 =
        sse(nlohmann::json{{"type", "response.output_item.added"},
                           {"output_index", 0},
                           {"item", {{"id", "rs_b"},
                                     {"type", "reasoning"},
                                     {"summary", nlohmann::json::array()}}}}) +
        sse(nlohmann::json{{"type", "response.reasoning_text.delta"},
                           {"item_id", "rs_b"},
                           {"delta", "raw thinking"}}) +
        sse(nlohmann::json{{"type", "response.reasoning_summary_text.delta"},
                           {"item_id", "rs_b"},
                           {"delta", "tidy summary"}}) +
        sse(nlohmann::json{{"type", "response.completed"},
                           {"response", {{"id", "r"}}}});
    read_all(io2, both, chunk2);
    BOOST_REQUIRE(both.response().reasoning);
    BOOST_CHECK_EQUAL(both.response().reasoning->raw, "raw thinking");
}

// The multi-part case: summary parts stay separate (keyed by their index),
// a part's .done text is authoritative for THAT part only, and the assembled
// fallback joins parts in index order.
BOOST_AUTO_TEST_CASE(multi_part_summaries_stay_separate_and_join) {
    asio::io_context io;
    ResponsesReader reader(io.get_executor());
    const std::string chunk =
        sse(nlohmann::json{{"type", "response.output_item.added"},
                           {"output_index", 0},
                           {"item", {{"id", "rs_m"},
                                     {"type", "reasoning"},
                                     {"summary", nlohmann::json::array()}}}}) +
        sse(nlohmann::json{{"type", "response.reasoning_summary_part.added"},
                           {"item_id", "rs_m"},
                           {"summary_index", 0}}) +
        sse(nlohmann::json{{"type", "response.reasoning_summary_text.delta"},
                           {"item_id", "rs_m"},
                           {"content_index", 0},
                           {"delta", "Need to check"}}) +
        sse(nlohmann::json{{"type", "response.reasoning_summary_text.delta"},
                           {"item_id", "rs_m"},
                           {"content_index", 0},
                           {"delta", " the records."}}) +
        sse(nlohmann::json{{"type", "response.reasoning_summary_part.added"},
                           {"item_id", "rs_m"},
                           {"summary_index", 1}}) +
        sse(nlohmann::json{{"type", "response.reasoning_summary_text.delta"},
                           {"item_id", "rs_m"},
                           {"content_index", 1},
                           {"delta", "Then compare."}}) +
        // .done texts are authoritative per part; arrival order (1 before 0)
        // must not affect the index-ordered join.
        sse(nlohmann::json{{"type", "response.reasoning_summary_part.done"},
                           {"item_id", "rs_m"},
                           {"summary_index", 1},
                           {"part", {{"type", "summary_text"},
                                     {"text", "Then compare them."}}}}) +
        sse(nlohmann::json{{"type", "response.reasoning_summary_part.done"},
                           {"item_id", "rs_m"},
                           {"summary_index", 0},
                           {"part", {{"type", "summary_text"},
                                     {"text", "Need to check the tables."}}}}) +
        sse(nlohmann::json{{"type", "response.completed"},
                           {"response", {{"id", "r"}}}});
    read_all(io, reader, chunk);
    BOOST_REQUIRE(reader.response().reasoning);
    BOOST_CHECK_EQUAL(reader.response().reasoning->raw,
                      "Need to check the tables.\n\nThen compare them.");
}

// The truncated-stream leniency _item() documents: deltas that arrived
// without their output_item.added/done are salvaged by best-effort routing
// instead of dropping the bytes.
BOOST_AUTO_TEST_CASE(untyped_item_bytes_are_salvaged) {
    // Text deltas with an item_id but no item lifecycle events at all.
    asio::io_context io;
    ResponsesReader reader(io.get_executor());
    const std::string chunk =
        sse(nlohmann::json{{"type", "response.output_text.delta"},
                           {"item_id", "orphan"},
                           {"output_index", 0},
                           {"delta", "the full "}}) +
        sse(nlohmann::json{{"type", "response.output_text.delta"},
                           {"item_id", "orphan"},
                           {"output_index", 0},
                           {"delta", "answer"}}) +
        sse(nlohmann::json{{"type", "response.completed"},
                           {"response", {{"id", "r"}}}});
    read_all(io, reader, chunk);
    BOOST_CHECK_EQUAL(reader.response().content.raw, "the full answer");

    // Argument deltas alone make the slot a salvaged function call.
    asio::io_context io2;
    ResponsesReader calls(io2.get_executor());
    const std::string chunk2 =
        sse(nlohmann::json{{"type", "response.function_call_arguments.delta"},
                           {"item_id", "fc_orphan"},
                           {"output_index", 0},
                           {"delta", "{\"city\":"}}) +
        sse(nlohmann::json{{"type", "response.function_call_arguments.delta"},
                           {"item_id", "fc_orphan"},
                           {"output_index", 0},
                           {"delta", "\"London\"}"}}) +
        sse(nlohmann::json{{"type", "response.completed"},
                           {"response", {{"id", "r"}}}});
    read_all(io2, calls, chunk2);
    const auto& salvaged = calls.response();
    BOOST_REQUIRE_EQUAL(salvaged.invokes->size(), 1u);
    BOOST_CHECK_EQUAL((*salvaged.invokes)[0].arguments["city"], "London");
}

BOOST_AUTO_TEST_CASE(refusal_placement_depends_on_accompanying_text) {
    // Refusal-only response: the refusal IS the content.
    asio::io_context io;
    ResponsesReader reader(io.get_executor());
    const std::string refusal_only =
        sse(nlohmann::json{{"type", "response.output_item.added"},
                           {"output_index", 0},
                           {"item", {{"id", "m"},
                                     {"type", "message"},
                                     {"role", "assistant"},
                                     {"content", nlohmann::json::array()}}}}) +
        sse(nlohmann::json{{"type", "response.refusal.delta"},
                           {"item_id", "m"},
                           {"delta", "I cannot help with that."}}) +
        sse(nlohmann::json{{"type", "response.completed"},
                           {"response", {{"id", "r"}}}});
    read_all(io, reader, refusal_only);
    const auto& response = reader.response();
    BOOST_CHECK_EQUAL(response.content.raw, "I cannot help with that.");
    BOOST_CHECK(!response.content.extras);

    // Refusal alongside output text: it rides in content.extras, kept out of
    // the visible text.
    asio::io_context io2;
    ResponsesReader mixed(io2.get_executor());
    const std::string refusal_and_text =
        sse(nlohmann::json{{"type", "response.output_item.added"},
                           {"output_index", 0},
                           {"item", {{"id", "m"},
                                     {"type", "message"},
                                     {"role", "assistant"},
                                     {"content", nlohmann::json::array()}}}}) +
        sse(nlohmann::json{{"type", "response.output_text.delta"},
                           {"item_id", "m"},
                           {"delta", "Partial answer."}}) +
        sse(nlohmann::json{{"type", "response.refusal.delta"},
                           {"item_id", "m"},
                           {"delta", "...but no more."}}) +
        sse(nlohmann::json{{"type", "response.completed"},
                           {"response", {{"id", "r"}}}});
    read_all(io2, mixed, refusal_and_text);
    const auto& mixed_response = mixed.response();
    BOOST_CHECK_EQUAL(mixed_response.content.raw, "Partial answer.");
    BOOST_REQUIRE(mixed_response.content.extras);
    BOOST_CHECK_EQUAL((*mixed_response.content.extras)["refusal"],
                      "...but no more.");
}

BOOST_AUTO_TEST_CASE(ignored_categories_leave_the_assembled_record_untouched) {
    asio::io_context io;
    ResponsesReader reader(io.get_executor());
    const std::string chunk =
        sse(nlohmann::json{{"type", "response.created"},
                           {"response", {{"id", "r"}}}}) +
        sse(nlohmann::json{{"type", "response.web_search_call.in_progress"},
                           {"item_id", "ws_1"},
                           {"output_index", 0}}) +
        sse(nlohmann::json{{"type", "response.output_item.added"},
                           {"output_index", 1},
                           {"item", {{"id", "fs_1"},
                                     {"type", "file_search_call"}}}}) +
        sse(nlohmann::json{{"type", "response.completed"},
                           {"response", {{"id", "r"}}}});
    read_all(io, reader, chunk);
    BOOST_CHECK(reader.status() == StreamStatus::Completed);
    const auto& response = reader.response();
    BOOST_CHECK_EQUAL(response.content.raw, "");
    BOOST_CHECK(!response.invokes);
}

// --- terminal states -------------------------------------------------------------

BOOST_AUTO_TEST_CASE(terminal_states_map_to_statuses_and_details) {
    const struct {
        const char* label;
        nlohmann::json event;
        StreamStatus status;
    } cases[] = {
        {"incomplete",
         nlohmann::json{{"type", "response.incomplete"},
                        {"response", {{"id", "r"},
                                      {"incomplete_details", {{"reason", "max_output_tokens"}}}}}},
         StreamStatus::Incomplete},
        {"failed",
         nlohmann::json{{"type", "response.failed"},
                        {"response", {{"id", "r"},
                                      {"error", {{"code", "server_error"},
                                                 {"message", "boom"}}}}}},
         StreamStatus::Failed},
        {"error",
         nlohmann::json{{"type", "error"},
                        {"code", "rate_limited"},
                        {"message", "slow down"}},
         StreamStatus::Errored},
    };

    for (const auto& item : cases) {
        asio::io_context io;
        ResponsesReader reader(io.get_executor());
        auto deltas = read_all(io, reader, sse(item.event));
        BOOST_REQUIRE_EQUAL(deltas.size(), 1u);
        BOOST_CHECK_MESSAGE(reader.finished(), item.label);
        BOOST_CHECK_MESSAGE(reader.status() == item.status, item.label);

        const auto& response = reader.response();
        BOOST_REQUIRE(response.extras);
        const auto& extras = *response.extras;
        if (item.status == StreamStatus::Incomplete) {
            BOOST_CHECK_EQUAL(extras["incomplete_details"]["reason"],
                              "max_output_tokens");
        } else if (item.status == StreamStatus::Errored) {
            BOOST_CHECK_EQUAL(extras["error"]["code"], "rate_limited");
        } else {
            BOOST_CHECK_EQUAL(extras["error"]["code"], "server_error");
        }
    }
}

// --- hooks (the PeekingHandler replacement) --------------------------------------

// A reusable monitor functor — the many-to-many case: one functor type,
// several reader instances.
struct Recorder {
    std::vector<ResponsesDelta>* out;
    void operator()(const ResponsesDelta& delta) const {
        out->push_back(delta);
    }
};

BOOST_AUTO_TEST_CASE(hooks_observe_every_delta_without_consuming) {
    asio::io_context io;
    ResponsesReader reader(io.get_executor());
    std::vector<ResponsesDelta> seen_first, seen_second;
    reader.add_hook(Recorder{&seen_first});
    reader.add_hook(Recorder{&seen_second});

    auto deltas = read_all(io, reader, text_stream());

    // Both hooks saw every delta — the terminal marker included — in order,
    // identical to what next() delivered.
    BOOST_REQUIRE_EQUAL(seen_first.size(), 10u);
    BOOST_REQUIRE_EQUAL(seen_second.size(), 10u);
    for (std::size_t index = 0; index < deltas.size(); ++index) {
        BOOST_CHECK(seen_first[index].kind == deltas[index].kind);
        BOOST_CHECK_EQUAL(seen_first[index].text, deltas[index].text);
        BOOST_CHECK_EQUAL(seen_first[index].item_id, deltas[index].item_id);
        BOOST_CHECK_EQUAL(seen_second[index].text, deltas[index].text);
    }
    // Observation did not disturb the consumption: the record assembled.
    BOOST_CHECK_EQUAL(reader.response().content.raw, "The answer is 42");

    // "Print the thinking process": a monitor filtering the reasoning
    // channels — and the same functor type on a second reader.
    std::string thinking;
    asio::io_context io2;
    ResponsesReader other(io2.get_executor());
    std::vector<ResponsesDelta> other_seen;
    other.add_hook(Recorder{&other_seen});   // same functor, other reader
    other.add_hook([&thinking](const ResponsesDelta& delta) {
        if (delta.kind == DeltaKind::ReasoningText ||
            delta.kind == DeltaKind::ReasoningSummary) {
            thinking += delta.text;
        }
    });
    read_all(io2, other, text_stream());
    BOOST_CHECK_EQUAL(other_seen.size(), 10u);
    BOOST_CHECK_EQUAL(other_seen[3].text, "The");
    BOOST_CHECK_EQUAL(other.response().content.raw, "The answer is 42");
}

// Sync hooks run first, then async hooks, each in registration order, and
// an async hook's I/O suspends the read: the stream cannot race ahead of
// it (inline co_await — the ordering guarantee the old producer-side
// peekers had, now with awaitable bodies).
BOOST_AUTO_TEST_CASE(async_hooks_run_inline_in_order) {
    asio::io_context io;
    ResponsesReader reader(io.get_executor());

    std::vector<std::string> order;
    reader.add_hook([&order](const ResponsesDelta& delta) {
        order.push_back("sync:" + delta.text);
    });
    reader.add_async_hook(
        [&order](const ResponsesDelta& delta) -> asio::awaitable<void> {
            asio::steady_timer timer(co_await asio::this_coro::executor);
            timer.expires_after(std::chrono::milliseconds(1));
            co_await timer.async_wait(asio::use_awaitable);
            order.push_back("async:" + delta.text);
        });

    const std::string chunk =
        sse(nlohmann::json{{"type", "response.created"},
                           {"response", {{"id", "r"}}}}) +
        sse(nlohmann::json{{"type", "response.output_text.delta"},
                           {"item_id", "m"},
                           {"delta", "hi"}}) +
        sse(nlohmann::json{{"type", "response.completed"},
                           {"response", {{"id", "r"}}}});
    read_all(io, reader, chunk);

    // Strict interleaving: no delta's async hook runs before the previous
    // delta's has completed.
    const std::vector<std::string> expected = {
        "sync:response.created", "async:response.created",
        "sync:hi",               "async:hi",
        "sync:response.completed", "async:response.completed",
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(
        order.begin(), order.end(), expected.begin(), expected.end());
}

// A throwing hook is a consumer-side bug: next() drives the handler to
// ERROR (aborting the producer too), then rethrows — the fault surfaces at
// the consume loop, and the stream reports Aborted.
BOOST_AUTO_TEST_CASE(a_throwing_hook_kills_the_stream_as_error) {
    asio::io_context io;
    ResponsesReader reader(io.get_executor());
    reader.add_hook([](const ResponsesDelta&) {
        throw std::runtime_error("hook boom");
    });

    BOOST_CHECK_THROW(read_all(io, reader, text_stream()), std::runtime_error);

    BOOST_CHECK(reader.finished());
    BOOST_CHECK(reader.handler()->get_state() == endpoint::SSEHandlerState::ERROR);
    BOOST_CHECK(reader.status() == StreamStatus::Aborted);
}

// --- aborts (the centralized exception policy) ------------------------------------

// A producer-side fault: the channel closes through finish(ERROR) — the
// reader absorbs the resulting SSEAborted as a stream END, not an error,
// and reports Aborted; nothing was assembled.
BOOST_AUTO_TEST_CASE(producer_fault_ends_the_stream_as_aborted) {
    asio::io_context io;
    ResponsesReader reader(io.get_executor());

    const std::string one_event =
        sse(nlohmann::json{{"type", "response.created"},
                           {"response", {{"id", "r"}}}});
    std::vector<ResponsesDelta> out;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            out.push_back(*co_await reader.next());   // event delivered
            auto after = co_await reader.next();      // SSEAborted absorbed
            BOOST_CHECK(!after.has_value());
        },
        asio::detached);
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            co_await reader.handler()->put(one_event);
            reader.handler()->finish(endpoint::SSEHandlerState::ERROR);
        },
        asio::detached);
    io.run();

    BOOST_REQUIRE_EQUAL(out.size(), 1u);
    BOOST_CHECK(reader.finished());
    BOOST_CHECK(reader.status() == StreamStatus::Aborted);
    BOOST_CHECK_EQUAL(reader.response().content.raw, "");   // no assembly
}

// A consumer that will not drain to the terminal event calls abort(): the
// producer stops cooperatively (its pending put aborts), next() returns
// nullopt, and the status is Aborted.
BOOST_AUTO_TEST_CASE(consumer_abort_ends_the_stream_as_aborted) {
    asio::io_context io;
    ResponsesReader reader(io.get_executor());

    const std::string three_events =
        sse(nlohmann::json{{"type", "response.created"},
                           {"response", {{"id", "r"}}}}) +
        sse(nlohmann::json{{"type", "response.output_text.delta"},
                           {"item_id", "m"},
                           {"delta", "hi"}}) +
        sse(nlohmann::json{{"type", "response.output_text.delta"},
                           {"item_id", "m"},
                           {"delta", " there"}});
    std::size_t seen = 0;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto first = co_await reader.next();
            BOOST_CHECK(first.has_value());
            ++seen;
            reader.abort();                            // mid-stream stop
            auto after = co_await reader.next();
            BOOST_CHECK(!after.has_value());
            auto again = co_await reader.next();
            BOOST_CHECK(!again.has_value());           // idempotent
        },
        asio::detached);
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                co_await reader.handler()->put(three_events);
            } catch (const endpoint::SSEAborted&) {
                // Cooperative stop: the reader closed the channel.
            }
        },
        asio::detached);
    io.run();

    BOOST_CHECK_EQUAL(seen, 1u);
    BOOST_CHECK(reader.finished());
    BOOST_CHECK(reader.status() == StreamStatus::Aborted);
    BOOST_CHECK(reader.handler()->get_state() == endpoint::SSEHandlerState::ERROR);
}

// --- the drain convenience ---------------------------------------------------------

BOOST_AUTO_TEST_CASE(consume_drains_and_returns_the_assembled_item) {
    asio::io_context io;
    ResponsesReader reader(io.get_executor());
    std::size_t hook_count = 0;
    reader.add_hook([&hook_count](const ResponsesDelta&) { ++hook_count; });

    std::optional<model_io::MessageItem> item;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            item = co_await reader.consume();
        },
        asio::detached);
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            co_await reader.handler()->put(text_stream());
        },
        asio::detached);
    io.run();

    BOOST_REQUIRE(item);
    BOOST_CHECK_EQUAL(item->content.raw, "The answer is 42");
    BOOST_CHECK_EQUAL(item->role, "assistant");
    BOOST_CHECK_EQUAL(hook_count, 10u);   // hooks still monitored the drain
    BOOST_CHECK(reader.finished());
    BOOST_CHECK(reader.status() == StreamStatus::Completed);
}

// --- review-fix regressions ---------------------------------------------------------

// A monitor dying on the LAST event must not corrupt the outcome: the
// record assembles (and the handler reports ERROR) BEFORE the hooks run on
// the terminal delta, so status()/response() stay coherent — Completed with
// the full text, not Completed beside an empty record.
BOOST_AUTO_TEST_CASE(a_hook_fault_on_the_terminal_delta_keeps_the_record_final) {
    asio::io_context io;
    ResponsesReader reader(io.get_executor());
    reader.add_hook([](const ResponsesDelta& delta) {
        if (is_terminal(delta)) throw std::runtime_error("terminal hook boom");
    });

    BOOST_CHECK_THROW(read_all(io, reader, text_stream()), std::runtime_error);

    BOOST_CHECK(reader.status() == StreamStatus::Completed);
    BOOST_CHECK_EQUAL(reader.response().content.raw, "The answer is 42");
    BOOST_CHECK(reader.handler()->get_state() == endpoint::SSEHandlerState::ERROR);
}

// Only get()'s SSEAborted is a lifecycle end. A hook throwing SSEAborted is
// a fault like any other: the handler is driven to ERROR, the exception
// propagates — it is not misreported as a cooperative abort.
BOOST_AUTO_TEST_CASE(a_hook_throwing_sseaborted_is_a_fault_not_an_abort) {
    asio::io_context io;
    ResponsesReader reader(io.get_executor());
    reader.add_hook([](const ResponsesDelta&) {
        throw endpoint::SSEAborted(
            "hook-thrown abort", endpoint::SSEHandlerState::ERROR);
    });

    BOOST_CHECK_THROW(read_all(io, reader, text_stream()), endpoint::SSEAborted);

    BOOST_CHECK(reader.finished());
    BOOST_CHECK(reader.handler()->get_state() == endpoint::SSEHandlerState::ERROR);
    BOOST_CHECK(reader.status() == StreamStatus::Aborted);
}

// A data-less `event: error` frame is terminal on the wire view (its Marker
// text) but carries no payload to fold — it must still classify as a
// server-side Errored, not fall through to a transport Aborted.
BOOST_AUTO_TEST_CASE(dataless_error_frame_reports_errored_not_aborted) {
    asio::io_context io;
    ResponsesReader reader(io.get_executor());

    auto deltas = read_all(io, reader, "event: error\n\n");
    BOOST_REQUIRE_EQUAL(deltas.size(), 1u);
    BOOST_CHECK(reader.finished());
    BOOST_CHECK(reader.status() == StreamStatus::Errored);
}

// Orphan items (no output_item.added/done — the leniency path) order by the
// wire output_index the deltas themselves carry; arrival order must not win
// (positional tool-result correlation depends on it).
BOOST_AUTO_TEST_CASE(orphan_items_order_by_wire_output_index) {
    asio::io_context io;
    ResponsesReader reader(io.get_executor());
    const std::string chunk =
        sse(nlohmann::json{{"type", "response.function_call_arguments.delta"},
                           {"item_id", "fc_b"},
                           {"output_index", 1},
                           {"delta", "{\"city\":\"B\"}"}}) +
        sse(nlohmann::json{{"type", "response.function_call_arguments.delta"},
                           {"item_id", "fc_a"},
                           {"output_index", 0},
                           {"delta", "{\"city\":\"A\"}"}}) +
        sse(nlohmann::json{{"type", "response.completed"},
                           {"response", {{"id", "r"}}}});
    read_all(io, reader, chunk);

    const auto& invokes = *reader.response().invokes;
    BOOST_REQUIRE_EQUAL(invokes.size(), 2u);
    BOOST_CHECK_EQUAL(invokes[0].arguments["city"], "A");   // index 0 first
    BOOST_CHECK_EQUAL(invokes[1].arguments["city"], "B");
}

// --- pump(): the producer-side wrapper ----------------------------------------------

// A helper building one SSE POST request for the loopback server.
static endpoint::ModelRequestInterpreter::HttpRequest sse_post() {
    http::request<http::string_body> request{http::verb::post, "/v1/responses", 11};
    request.set(http::field::host, "localhost");
    request.set(http::field::accept, "text/event-stream");
    return request;
}

// Drive one reader against a one-shot loopback server: pump() as the
// producer (one co_spawn), taking the request driver as a parameter, and the
// canonical next() loop as the consumer. Returns once BOTH sides have ended —
// with pump() finishing the handler on exit, a stream that ends without its
// terminal event cannot hang the consumer (the pre-pump wiring did exactly
// that).
struct PumpResult {
    std::vector<ResponsesDelta> deltas;
    std::optional<HttpRequestException::Stage> pump_stage;
    std::string pump_error;
};

template<typename Driver>
static PumpResult run_pumped(
    Driver driver, unsigned short port, ResponsesReader& reader,
    asio::io_context& io) {
    PumpResult result;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                auto stream = co_await endpoint::create_http_connection_stream(
                    "127.0.0.1", std::to_string(port));
                co_await reader.pump(driver, std::move(stream), sse_post());
            } catch (const HttpRequestException& error) {
                result.pump_stage = error.stage();
                result.pump_error = error.what();
            }
        },
        asio::detached);
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            while (auto delta = co_await reader.next()) {
                result.deltas.push_back(std::move(*delta));
            }
        },
        asio::detached);
    io.run();
    return result;
}

BOOST_AUTO_TEST_CASE(pump_completes_the_stream_with_a_terminal_event) {
    loopback::OneShotServer server([](tcp::socket& socket) {
        loopback::serve_fixed_response(socket, http::status::ok, text_stream());
    });
    const unsigned short port = server.wait_listening();

    asio::io_context io;
    ResponsesReader reader(io.get_executor());
    PumpResult result = run_pumped(
        &endpoint::sse_request<ResponsesDelta, endpoint::http_stream>,
        port, reader, io);
    server.join();

    BOOST_REQUIRE(!result.pump_stage);
    BOOST_REQUIRE_EQUAL(result.deltas.size(), 10u);
    BOOST_CHECK(reader.status() == StreamStatus::Completed);
    BOOST_CHECK_EQUAL(reader.response().content.raw, "The answer is 42");
    // The reader finished DONE on the terminal marker; pump's exit sees a
    // non-RUNNING handler and leaves that state alone.
    BOOST_CHECK(reader.handler()->get_state() == endpoint::SSEHandlerState::DONE);
}

BOOST_AUTO_TEST_CASE(pump_runs_equally_with_the_bounded_driver) {
    // Same reader, same canonical wiring, different driver flavour:
    // HttpRequestDriver instead of sse_request. pump() depends on the
    // RequestDriver contract, not the streaming flavour — and the whole SSE
    // body arriving as one put() frames into exactly the same deltas.
    loopback::OneShotServer server([](tcp::socket& socket) {
        loopback::serve_fixed_response(socket, http::status::ok, text_stream());
    });
    const unsigned short port = server.wait_listening();

    asio::io_context io;
    ResponsesReader reader(io.get_executor());
    PumpResult result = run_pumped(
        endpoint::HttpRequestDriver<ResponsesDelta, endpoint::http_stream>{},
        port, reader, io);
    server.join();

    BOOST_REQUIRE(!result.pump_stage);
    if (result.pump_stage) BOOST_TEST_MESSAGE("pump error: " << result.pump_error);
    BOOST_REQUIRE_EQUAL(result.deltas.size(), 10u);
    BOOST_CHECK(reader.status() == StreamStatus::Completed);
    BOOST_CHECK_EQUAL(reader.response().content.raw, "The answer is 42");
    BOOST_CHECK(reader.handler()->get_state() == endpoint::SSEHandlerState::DONE);
}

BOOST_AUTO_TEST_CASE(pump_wakes_the_consumer_when_the_stream_ends_without_terminal) {
    // Server sends one delta, then closes: no response.completed ever
    // arrives. The driver returns WITHOUT finishing the handler (its
    // contract) — pump() finishes it, so next() ends in nullopt instead of
    // blocking forever, and the reader classifies the end as Aborted.
    loopback::OneShotServer server([](tcp::socket& socket) {
        loopback::serve_fixed_response(
            socket, http::status::ok,
            "data: {\"type\":\"response.output_text.delta\","
            "\"item_id\":\"m\",\"delta\":\"hi\"}\n\n");
    });
    const unsigned short port = server.wait_listening();

    asio::io_context io;
    ResponsesReader reader(io.get_executor());
    PumpResult result = run_pumped(
        &endpoint::sse_request<ResponsesDelta, endpoint::http_stream>,
        port, reader, io);
    server.join();

    BOOST_REQUIRE(!result.pump_stage);
    BOOST_REQUIRE_EQUAL(result.deltas.size(), 1u);
    BOOST_CHECK(reader.finished());
    BOOST_CHECK(reader.status() == StreamStatus::Aborted);
}

BOOST_AUTO_TEST_CASE(pump_surfaces_rejection_and_wakes_the_consumer) {
    // A non-200 reply throws on the producer side — pump() finishes the
    // handler FIRST, so the consumer wakes (Aborted) instead of hanging
    // while the HttpRequestException propagates.
    loopback::OneShotServer server([](tcp::socket& socket) {
        loopback::serve_fixed_response(
            socket, http::status::internal_server_error, "data: ignored\n\n");
    });
    const unsigned short port = server.wait_listening();

    asio::io_context io;
    ResponsesReader reader(io.get_executor());
    PumpResult result = run_pumped(
        &endpoint::sse_request<ResponsesDelta, endpoint::http_stream>,
        port, reader, io);
    server.join();

    BOOST_REQUIRE(result.pump_stage);
    BOOST_CHECK(*result.pump_stage ==
                HttpRequestException::Stage::HandleResponse);
    BOOST_CHECK(result.deltas.empty());
    BOOST_CHECK(reader.finished());
    BOOST_CHECK(reader.status() == StreamStatus::Aborted);
}
