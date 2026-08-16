// Deterministic, offline tests for the Responses-API stream handler and the
// endpoint-level PeekingHandler. A plain boost::asio::io_context drives the
// put/get coroutines (no sockets, no TLS), mirroring
// test_sse_response_handler.cpp: the channel is zero-capacity rendezvous, so
// consumers are pre-spawned to pair with each put() — one delta per SSE
// event, and `expect` counts events exactly.
#define BOOST_TEST_MODULE responses_stream
#include <boost/test/unit_test.hpp>

#include <boost/asio.hpp>

#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "endpoint/peeking_handler.hpp"
#include "endpoint/request.hpp"
#include "endpoint/responses/delta.hpp"
#include "endpoint/responses/stream_handler.hpp"

namespace asio = boost::asio;
namespace http = boost::beast::http;

using endpoint::PeekingHandler;
using endpoint::responses::DeltaKind;
using endpoint::responses::ResponsesDelta;
using endpoint::responses::ResponsesStreamHandler;
using endpoint::responses::StreamStatus;

// --- helpers ------------------------------------------------------------------

// Pump the io_context once: run ready handlers without blocking.
static void pump(asio::io_context& io) {
    io.poll();
    io.restart();
}

// Drive put(chunk) with `expect` pre-spawned get() consumers; return the
// deltas in delivery order. Rethrows any get() exception.
template<typename Handler>
static std::vector<ResponsesDelta> exchange(
    asio::io_context& io, Handler& handler,
    std::string_view chunk, std::size_t expect) {
    std::vector<std::optional<ResponsesDelta>> received(expect);
    std::vector<std::optional<std::exception_ptr>> errors(expect);
    for (std::size_t index = 0; index < expect; ++index) {
        asio::co_spawn(
            io,
            [&, index]() -> asio::awaitable<void> {
                try {
                    received[index] = co_await handler.get();
                } catch (...) {
                    errors[index] = std::current_exception();
                }
            },
            asio::detached);
    }
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> { co_await handler.put(chunk); },
        asio::detached);
    pump(io);

    std::vector<ResponsesDelta> out;
    for (std::size_t index = 0; index < expect; ++index) {
        if (errors[index]) std::rethrow_exception(*errors[index]);
        out.push_back(std::move(*received[index]));
    }
    return out;
}

// One SSE frame carrying the given event JSON.
static std::string sse(const nlohmann::json& event) {
    return "event: " + event.at("type").get<std::string>() + "\ndata: " +
           event.dump() + "\n\n";
}

static std::string sse_raw(const std::string& data) {
    return "data: " + data + "\n\n";
}

// The canonical happy-path text stream: 10 frames, 10 deltas.
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

// --- text stream ---------------------------------------------------------------

BOOST_AUTO_TEST_CASE(text_stream_decodes_and_assembles) {
    asio::io_context io;
    ResponsesStreamHandler handler(io.get_executor());

    auto deltas = exchange(io, handler, text_stream(), 10);
    BOOST_REQUIRE_EQUAL(deltas.size(), 10u);

    // Lifecycle frames are Markers; frames 3-5 are the Text increments.
    for (std::size_t index = 0; index < deltas.size(); ++index) {
        const bool is_text = (index >= 3 && index < 6);
        BOOST_CHECK_MESSAGE(
            deltas[index].kind == (is_text ? DeltaKind::Text : DeltaKind::Marker),
            "index " << index << " kind " << deltas[index].kind);
    }
    std::string streamed;
    for (std::size_t index = 3; index < 6; ++index) {
        BOOST_CHECK_EQUAL(deltas[index].item_id, "msg_1");
        BOOST_CHECK_EQUAL(deltas[index].content_index.value_or(999),
                          std::size_t{0});
        streamed += deltas[index].text;
    }
    BOOST_CHECK_EQUAL(streamed, "The answer is 42");

    // output_text.done is a Marker with the authoritative text — NOT a
    // second Text delta (consumers would print everything twice).
    BOOST_CHECK(deltas[6].kind == DeltaKind::Marker);
    BOOST_CHECK_EQUAL(deltas[6].text, "response.output_text.done");

    // Terminal bookkeeping.
    BOOST_CHECK(ResponsesStreamHandler::is_terminal(deltas[9]));
    BOOST_CHECK(!ResponsesStreamHandler::is_terminal(deltas[0]));
    BOOST_CHECK(handler.finished());
    BOOST_CHECK(handler.status() == StreamStatus::Completed);

    // The assembled contract record.
    const auto& response = handler.response();
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

// --- function calling ----------------------------------------------------------

BOOST_AUTO_TEST_CASE(function_call_stream_assembles_invokes) {
    asio::io_context io;
    ResponsesStreamHandler handler(io.get_executor());

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

    auto deltas = exchange(io, handler, chunk, 12);
    BOOST_REQUIRE_EQUAL(deltas.size(), 12u);

    // Reasoning channel deltas are ReasoningText; done overwrites silently.
    BOOST_CHECK(deltas[2].kind == DeltaKind::ReasoningText);
    BOOST_CHECK(deltas[3].kind == DeltaKind::ReasoningText);
    BOOST_CHECK(deltas[4].kind == DeltaKind::Marker);
    // Arguments stream as ToolCallArgs fragments.
    BOOST_CHECK(deltas[7].kind == DeltaKind::ToolCallArgs);
    BOOST_CHECK_EQUAL(deltas[7].text, "{\"city\":");
    BOOST_CHECK(deltas[8].kind == DeltaKind::ToolCallArgs);
    BOOST_CHECK(deltas[9].kind == DeltaKind::Marker);
    BOOST_CHECK(handler.status() == StreamStatus::Completed);

    const auto& response = handler.response();
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
    ResponsesStreamHandler handler(io.get_executor());

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
    exchange(io, handler, chunk, 3);

    const auto& invokes = *handler.response().invokes;
    BOOST_REQUIRE_EQUAL(invokes.size(), 1u);
    BOOST_CHECK_EQUAL(invokes[0].arguments, "{not json");   // kept verbatim
}

// --- dual reasoning channels ----------------------------------------------------

BOOST_AUTO_TEST_CASE(reasoning_summary_and_text_are_distinct_channels) {
    asio::io_context io;
    ResponsesStreamHandler summary_only(io.get_executor());
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
    auto deltas = exchange(io, summary_only, chunk, 4);
    BOOST_CHECK(deltas[2].kind == DeltaKind::ReasoningSummary);
    const auto& response = summary_only.response();
    BOOST_REQUIRE(response.reasoning);
    BOOST_CHECK_EQUAL(response.reasoning->raw, "Short version.");   // fallback

    // With both channels present the raw thinking wins over the summary.
    asio::io_context io2;
    ResponsesStreamHandler both(io2.get_executor());
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
    exchange(io2, both, chunk2, 4);
    BOOST_REQUIRE(both.response().reasoning);
    BOOST_CHECK_EQUAL(both.response().reasoning->raw, "raw thinking");
}

// The multi-part case: summary parts stay separate (keyed by their index),
// a part's .done text is authoritative for THAT part only, and the assembled
// fallback joins parts in index order. Before the fix all parts glued into
// one separator-less string and the last .done overwrote the whole thing.
BOOST_AUTO_TEST_CASE(multi_part_summaries_stay_separate_and_join) {
    asio::io_context io;
    ResponsesStreamHandler handler(io.get_executor());
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
    exchange(io, handler, chunk, 9);
    BOOST_REQUIRE(handler.response().reasoning);
    BOOST_CHECK_EQUAL(handler.response().reasoning->raw,
                      "Need to check the tables.\n\nThen compare them.");
}

// The truncated-stream leniency _item() documents: deltas that arrived
// without their output_item.added/done. _assemble used to drop those bytes
// entirely; now they are salvaged by best-effort routing.
BOOST_AUTO_TEST_CASE(untyped_item_bytes_are_salvaged) {
    // Text deltas with an item_id but no item lifecycle events at all.
    asio::io_context io;
    ResponsesStreamHandler handler(io.get_executor());
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
    exchange(io, handler, chunk, 3);
    BOOST_CHECK_EQUAL(handler.response().content.raw, "the full answer");

    // Argument deltas alone make the slot a salvaged function call.
    asio::io_context io2;
    ResponsesStreamHandler calls(io2.get_executor());
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
    exchange(io2, calls, chunk2, 3);
    const auto& salvaged = calls.response();
    BOOST_REQUIRE_EQUAL(salvaged.invokes->size(), 1u);
    BOOST_CHECK_EQUAL((*salvaged.invokes)[0].arguments["city"], "London");
}

// A comment-only frame (": keep-alive\n\n") between events is NOT an event:
// before the framing fix it leaked into the handler as a bogus Marker delta.
BOOST_AUTO_TEST_CASE(comment_frames_produce_no_deltas) {
    asio::io_context io;
    ResponsesStreamHandler handler(io.get_executor());
    const std::string chunk =
        sse(nlohmann::json{{"type", "response.created"},
                           {"response", {{"id", "r"}}}}) +
        ": keep-alive\n\n" +
        sse(nlohmann::json{{"type", "response.completed"},
                           {"response", {{"id", "r"}}}});
    auto deltas = exchange(io, handler, chunk, 2);
    BOOST_CHECK(deltas[0].kind == DeltaKind::Marker);
    BOOST_CHECK_EQUAL(deltas[0].text, "response.created");
    BOOST_CHECK(deltas[1].kind == DeltaKind::Marker);
    BOOST_CHECK_EQUAL(deltas[1].text, "response.completed");
}

// --- peeker ---------------------------------------------------------------------

// A reusable monitor functor — the many-to-many case: one functor type,
// several handler instances.
struct Recorder {
    std::vector<ResponsesDelta>* out;
    void operator()(const ResponsesDelta& delta) const {
        out->push_back(delta);
    }
};

BOOST_AUTO_TEST_CASE(peeking_handler_observes_without_consuming) {
    asio::io_context io;
    PeekingHandler<ResponsesStreamHandler> handler(io.get_executor());
    std::vector<ResponsesDelta> seen_first, seen_second;
    handler.add_peeker(Recorder{&seen_first});
    handler.add_peeker(Recorder{&seen_second});

    auto deltas = exchange(io, handler, text_stream(), 10);

    // Both peekers saw every delta, in order, identical to the stream.
    BOOST_REQUIRE_EQUAL(seen_first.size(), 10u);
    BOOST_REQUIRE_EQUAL(seen_second.size(), 10u);
    for (std::size_t index = 0; index < deltas.size(); ++index) {
        BOOST_CHECK(seen_first[index].kind == deltas[index].kind);
        BOOST_CHECK_EQUAL(seen_first[index].text, deltas[index].text);
        BOOST_CHECK_EQUAL(seen_first[index].item_id, deltas[index].item_id);
        BOOST_CHECK_EQUAL(seen_second[index].text, deltas[index].text);
    }

    // "Print the thinking process": a monitor filtering reasoning channels.
    std::string thinking;
    handler.add_peeker([&thinking](const ResponsesDelta& delta) {
        if (delta.kind == DeltaKind::ReasoningText ||
            delta.kind == DeltaKind::ReasoningSummary) {
            thinking += delta.text;
        }
    });
    asio::io_context io2;
    PeekingHandler<ResponsesStreamHandler> other(io2.get_executor());
    std::vector<ResponsesDelta> other_seen;
    other.add_peeker(Recorder{&other_seen});   // same functor, other handler
    auto other_deltas = exchange(io2, other, text_stream(), 10);
    BOOST_REQUIRE_EQUAL(other_seen.size(), other_deltas.size());
    BOOST_CHECK_EQUAL(other_seen[3].text, other_deltas[3].text);

    // The wrapper still exposes the wrapped handler's API.
    BOOST_CHECK(other.status() == StreamStatus::Completed);
    BOOST_CHECK_EQUAL(other.response().content.raw, "The answer is 42");
}

BOOST_AUTO_TEST_CASE(a_throwing_peeker_kills_the_stream_as_error) {
    asio::io_context io;
    PeekingHandler<ResponsesStreamHandler> handler(io.get_executor());
    handler.add_peeker([](const ResponsesDelta&) {
        throw std::runtime_error("peek boom");
    });

    // The peek runs inside _handle_message: put()'s catch(...) drives the
    // handler to ERROR and rethrows, exactly like a decode fault.
    std::optional<std::exception_ptr> put_error;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                co_await handler.put(text_stream());
            } catch (...) {
                put_error = std::current_exception();
            }
        },
        asio::detached);
    pump(io);

    BOOST_REQUIRE(put_error);
    try {
        std::rethrow_exception(*put_error);
        BOOST_FAIL("expected the peeker's exception to propagate");
    } catch (const std::runtime_error& error) {
        BOOST_CHECK_EQUAL(std::string(error.what()), "peek boom");
    }
    BOOST_CHECK(handler.get_state() == endpoint::SSEHandlerState::ERROR);

    std::optional<ResponsesDelta> received;
    std::optional<std::exception_ptr> get_error;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                received = co_await handler.get();
            } catch (...) {
                get_error = std::current_exception();
            }
        },
        asio::detached);
    pump(io);
    BOOST_CHECK(!received);
    BOOST_REQUIRE(get_error);
    try {
        std::rethrow_exception(*get_error);
    } catch (const endpoint::SSEAborted& aborted) {
        BOOST_CHECK(aborted.state() == endpoint::SSEHandlerState::ERROR);
    }
}

// --- explicit placeholders --------------------------------------------------------

BOOST_AUTO_TEST_CASE(unhandled_categories_surface_as_ignored_deltas) {
    asio::io_context io;
    ResponsesStreamHandler handler(io.get_executor());

    const std::string chunk =
        sse(nlohmann::json{{"type", "response.created"},
                           {"response", {{"id", "r"}}}}) +
        sse(nlohmann::json{{"type", "response.web_search_call.in_progress"},
                           {"item_id", "ws_1"},
                           {"output_index", 0}}) +
        sse(nlohmann::json{{"type", "response.web_search_call.completed"},
                           {"item_id", "ws_1"},
                           {"output_index", 0}}) +
        sse(nlohmann::json{{"type", "response.mcp_list_tools.completed"},
                           {"item_id", "mcp_1"},
                           {"output_index", 1}}) +
        sse(nlohmann::json{{"type", "response.output_text.annotation.added"},
                           {"item_id", "msg_1"},
                           {"annotation_index", 0}}) +
        sse(nlohmann::json{{"type", "response.output_item.added"},
                           {"output_index", 2},
                           {"item", {{"id", "fs_1"},
                                     {"type", "file_search_call"}}}}) +
        sse(nlohmann::json{{"type", "response.novel_event"},
                           {"item_id", "x"}}) +
        sse(nlohmann::json{{"type", "response.completed"},
                           {"response", {{"id", "r"}}}});

    auto deltas = exchange(io, handler, chunk, 8);
    // Every category names itself; unknown types are prefixed.
    BOOST_CHECK(deltas[1].kind == DeltaKind::Ignored);
    BOOST_CHECK_EQUAL(deltas[1].text, "response.web_search_call.in_progress");
    BOOST_CHECK(deltas[2].kind == DeltaKind::Ignored);
    BOOST_CHECK(deltas[3].kind == DeltaKind::Ignored);
    BOOST_CHECK(deltas[4].kind == DeltaKind::Ignored);
    BOOST_CHECK(deltas[5].kind == DeltaKind::Ignored);
    BOOST_CHECK_EQUAL(deltas[5].text, "response.output_item.added");
    BOOST_CHECK(deltas[6].kind == DeltaKind::Ignored);
    BOOST_CHECK_EQUAL(deltas[6].text, "unknown:response.novel_event");
    BOOST_CHECK(handler.status() == StreamStatus::Completed);

    // Ignored categories leave the assembled record untouched.
    const auto& response = handler.response();
    BOOST_CHECK_EQUAL(response.content.raw, "");
    BOOST_CHECK(!response.invokes);
}

// --- terminal states ---------------------------------------------------------------

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
        ResponsesStreamHandler handler(io.get_executor());
        exchange(io, handler, sse(item.event), 1);
        BOOST_CHECK_MESSAGE(handler.finished(), item.label);
        BOOST_CHECK_MESSAGE(handler.status() == item.status, item.label);

        const auto& response = handler.response();
        BOOST_REQUIRE(response.extras);
        const auto& extras = *response.extras;
        if (item.status == StreamStatus::Incomplete) {
            BOOST_CHECK_EQUAL(extras["incomplete_details"]["reason"],
                              "max_output_tokens");
        } else {
            if (item.status == StreamStatus::Errored) {
                BOOST_CHECK_EQUAL(extras["error"]["code"], "rate_limited");
            } else {
                BOOST_CHECK_EQUAL(extras["error"]["code"], "server_error");
            }
        }
    }
}

// --- leniency --------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(malformed_frames_are_ignored_without_killing_the_stream) {
    asio::io_context io;
    ResponsesStreamHandler handler(io.get_executor());

    // Garbage data, a type-less object, [DONE], and an event:-only frame all
    // surface as deltas — and the handler stays RUNNING throughout.
    std::vector<ResponsesDelta> deltas = exchange(
        io, handler, "data: not json at all\n\n", 1);
    BOOST_CHECK(deltas[0].kind == DeltaKind::Ignored);
    BOOST_CHECK_EQUAL(deltas[0].text, "unparsable-data");
    BOOST_CHECK(handler.get_state() == endpoint::SSEHandlerState::RUNNING);

    deltas = exchange(io, handler, sse_raw("{\"foo\":1}"), 1);
    BOOST_CHECK(deltas[0].kind == DeltaKind::Ignored);
    BOOST_CHECK_EQUAL(deltas[0].text, "missing-type");

    deltas = exchange(io, handler, sse_raw("[DONE]"), 1);
    BOOST_CHECK(deltas[0].kind == DeltaKind::Ignored);
    BOOST_CHECK_EQUAL(deltas[0].text, "[DONE]");

    deltas = exchange(io, handler, "event: response.created\n\n", 1);
    BOOST_CHECK(deltas[0].kind == DeltaKind::Marker);
    BOOST_CHECK_EQUAL(deltas[0].text, "response.created");

    // Multi-line data frames join per the SSE spec before parsing.
    deltas = exchange(
        io, handler,
        "data: {\"type\": \"response.output_text.delta\",\ndata: "
        "\"item_id\": \"m\", \"delta\": \"hi\"}\n\n",
        1);
    BOOST_CHECK(deltas[0].kind == DeltaKind::Text);
    BOOST_CHECK_EQUAL(deltas[0].text, "hi");

    // And a real event still decodes afterwards — the stream never died.
    deltas = exchange(
        io, handler,
        sse(nlohmann::json{{"type", "response.completed"},
                           {"response", {{"id", "r"}}}}),
        1);
    BOOST_CHECK(deltas[0].kind == DeltaKind::Marker);
    BOOST_CHECK(handler.status() == StreamStatus::Completed);
}

BOOST_AUTO_TEST_CASE(refusal_placement_depends_on_accompanying_text) {
    // Refusal-only response: the refusal IS the content.
    asio::io_context io;
    ResponsesStreamHandler handler(io.get_executor());
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
    auto deltas = exchange(io, handler, refusal_only, 3);
    BOOST_CHECK(deltas[1].kind == DeltaKind::Refusal);
    const auto& response = handler.response();
    BOOST_CHECK_EQUAL(response.content.raw, "I cannot help with that.");
    BOOST_CHECK(!response.content.extras);

    // Refusal alongside output text: it rides in content.extras, kept out of
    // the visible text.
    asio::io_context io2;
    ResponsesStreamHandler mixed(io2.get_executor());
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
    exchange(io2, mixed, refusal_and_text, 4);
    const auto& mixed_response = mixed.response();
    BOOST_CHECK_EQUAL(mixed_response.content.raw, "Partial answer.");
    BOOST_REQUIRE(mixed_response.content.extras);
    BOOST_CHECK_EQUAL((*mixed_response.content.extras)["refusal"],
                      "...but no more.");
}
