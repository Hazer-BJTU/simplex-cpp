#define BOOST_TEST_MODULE responses_model
#include <boost/test/unit_test.hpp>

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "endpoint/http_request_exception.hpp"
#include "llm/compat/responses/interpreter.hpp"
#include "llm/compat/responses/model.hpp"
#include "loopback_server.hpp"

namespace asio = boost::asio;
namespace http = boost::beast::http;

namespace {

class FixtureDialect final : public llm::responses::ResponsesDialect {
public:
    model_io::ModelEndpoint default_endpoint() const override {
        model_io::ModelEndpoint endpoint;
        endpoint.base_url = "https://default.invalid/api";
        endpoint.request_path = "/responses";
        endpoint.auth.scheme = model_io::AuthScheme::Bearer;
        return endpoint;
    }

    void transform_request(nlohmann::json& body) const override {
        body["dialect_marker"] = true;
    }

    nlohmann::json normalize_event(nlohmann::json event) const override {
        if (event.value("type", std::string()) == "vendor.completed") {
            event["type"] = "response.completed";
        }
        return event;
    }
};

class FixtureModel final : public llm::responses::ResponsesModel {
public:
    FixtureModel(asio::any_io_executor executor, nlohmann::json config)
        : ResponsesModel(std::move(executor), std::move(config),
                         std::make_shared<const FixtureDialect>()) {}
};

model_io::AgentInputState one_user_turn() {
    model_io::AgentInputState state;
    auto& item = state.turns.emplace_back().user_input;
    item.type = model_io::MessageItemType::UserInput;
    item.role = "user";
    item.content.emplace_back().raw = "hello";
    return state;
}

// Accepts up to `count` connections for a bounded time, closes each
// immediately, and reports how many actually arrived: a truncated
// (recoverable) exchange the retry budget would happily re-run, so the
// connection count IS the retry count. The deadline unblocks the accept
// loop when fewer connections than `count` arrive (the assertion itself —
// no retry may come) instead of blocking join() forever.
class TruncatingServer {
public:
    explicit TruncatingServer(int count)
        : _port_promise(std::make_shared<std::promise<unsigned short>>()),
          _done_promise(std::make_shared<std::promise<void>>()),
          _port(_port_promise->get_future()),
          _done(_done_promise->get_future()),
          _connections(std::make_shared<int>(0)),
          _thread([this, count] {
              try {
                  asio::io_context io;
                  asio::ip::tcp::acceptor acceptor(
                      io, asio::ip::tcp::endpoint(
                              asio::ip::address_v4::loopback(), 0));
                  _port_promise->set_value(acceptor.local_endpoint().port());

                  // A retry (if one wrongly happens) lands after the
                  // initial backoff; the deadline only bounds the wait for
                  // connections that must NOT come.
                  asio::steady_timer deadline(io, std::chrono::seconds(2));
                  deadline.async_wait(
                      [&acceptor](const boost::system::error_code&) {
                          acceptor.cancel();
                      });

                  const auto connections = _connections;
                  std::function<void()> accept_next = [&] {
                      auto socket =
                          std::make_shared<asio::ip::tcp::socket>(io);
                      acceptor.async_accept(
                          *socket, [&, socket](
                                       const boost::system::error_code& ec) {
                              if (ec) return;   // cancelled: time is up
                              ++*connections;
                              if (*connections < count) accept_next();
                          });
                  };
                  accept_next();
                  io.run();
                  _done_promise->set_value();
              } catch (...) {
                  try {
                      _done_promise->set_exception(std::current_exception());
                  } catch (...) {
                  }
              }
          }) {}

    unsigned short wait_listening() { return _port.get(); }
    int connections() const { return *_connections; }
    void join() {
        _thread.join();
        _done.get();
    }

private:
    std::shared_ptr<std::promise<unsigned short>> _port_promise;
    std::shared_ptr<std::promise<void>> _done_promise;
    std::future<unsigned short> _port;
    std::future<void> _done;
    std::shared_ptr<int> _connections;
    std::thread _thread;
};

} // namespace

BOOST_AUTO_TEST_CASE(build_merges_provider_defaults_and_strips_host_config) {
    asio::io_context io;
    FixtureModel model(io.get_executor(), nlohmann::json{
        {"provider", "fixture"},
        {"model", "fixture-model"},
        {"temperature", 0.2},
        {"retry", {{"max_attempts", 1}}},
        {"endpoint", {
            {"base_url", "http://127.0.0.1:8123"},
            {"auth", {{"api_key", "secret"}}},
        }},
    });

    BOOST_REQUIRE(model.build());
    BOOST_CHECK_EQUAL(model.endpoint().base_url, "http://127.0.0.1:8123");
    BOOST_CHECK_EQUAL(model.endpoint().request_path, "/responses");
    BOOST_CHECK_EQUAL(model.endpoint().auth.api_key, "secret");
    BOOST_CHECK_EQUAL(model.generation()["model"], "fixture-model");
    BOOST_CHECK_EQUAL(model.generation()["temperature"], 0.2);
    BOOST_CHECK(!model.generation().contains("endpoint"));
    BOOST_CHECK(!model.generation().contains("provider"));
    BOOST_CHECK(!model.generation().contains("retry"));
    BOOST_CHECK(model.build()); // lifecycle hook is idempotent
}

BOOST_AUTO_TEST_CASE(build_rejects_missing_model_or_host) {
    asio::io_context io;
    FixtureModel missing_model(io.get_executor(), nlohmann::json::object());
    BOOST_CHECK(!missing_model.build());

    FixtureModel missing_host(io.get_executor(), nlohmann::json{
        {"model", "fixture-model"},
        {"endpoint", {{"base_url", ""}}},
    });
    BOOST_CHECK(!missing_host.build());
}

BOOST_AUTO_TEST_CASE(interpreter_maps_multimodal_content_and_runs_dialect) {
    llm::responses::ResponsesInterpreter interpreter(
        std::make_shared<const FixtureDialect>());
    model_io::AgentInputState state;

    auto& message = state.turns.emplace_back().user_input;
    message.type = model_io::MessageItemType::UserInput;
    // Each entry's category is stated and decides its part kind: an inline PDF
    // is a file and a referenced PNG is an image, whichever encoding each uses.
    auto& binary = message.content.emplace_back();
    binary.type = model_io::ContentType::Binary;
    binary.raw = "ZmFrZS1wZGY=";
    binary.extras = nlohmann::json{{"filename", "sample.pdf"}};
    binary.modality = model_io::Modality::Document;
    auto& image = message.content.emplace_back();
    image.type = model_io::ContentType::ExternalRef;
    image.raw = "https://example.invalid/image.png";
    image.modality = model_io::Modality::Image;

    model_io::ModelEndpoint endpoint;
    endpoint.base_url = "https://example.invalid";
    endpoint.request_path = "/responses";
    const auto request = interpreter.build_request(
        state, endpoint, nlohmann::json{{"model", "fixture-model"}});
    const auto body = nlohmann::json::parse(request.body());

    BOOST_CHECK(body["dialect_marker"].get<bool>());
    BOOST_CHECK_EQUAL(body["input"][0]["content"][0]["type"], "input_file");
    BOOST_CHECK_EQUAL(body["input"][0]["content"][0]["file_data"],
                      "ZmFrZS1wZGY=");
    BOOST_REQUIRE_EQUAL(body["input"][0]["content"].size(), 2u);
    BOOST_CHECK_EQUAL(body["input"][0]["content"][1]["type"], "input_image");
    BOOST_CHECK_EQUAL(body["input"][0]["content"][1]["image_url"],
                      "https://example.invalid/image.png");
}

// The retired inference: a referenced text part is text, not an image.
BOOST_AUTO_TEST_CASE(interpreter_does_not_infer_a_category_from_the_encoding) {
    llm::responses::ResponsesInterpreter interpreter(
        std::make_shared<const FixtureDialect>());
    model_io::AgentInputState state;
    auto& message = state.turns.emplace_back().user_input;
    message.type = model_io::MessageItemType::UserInput;
    auto& part = message.content.emplace_back();
    part.type = model_io::ContentType::ExternalRef;
    part.raw = "https://example.invalid/notes.txt";
    part.modality = model_io::Modality::Text;

    model_io::ModelEndpoint endpoint;
    endpoint.base_url = "https://example.invalid";
    endpoint.request_path = "/responses";
    const auto request = interpreter.build_request(
        state, endpoint, nlohmann::json{{"model", "fixture-model"}});
    const auto body = nlohmann::json::parse(request.body());

    BOOST_CHECK_EQUAL(body["input"][0]["content"][0]["type"], "input_text");
    BOOST_CHECK_EQUAL(body["input"][0]["content"][0]["text"],
                      "https://example.invalid/notes.txt");
}

// Hard error #3: the input list carries text, images and files, so audio and
// video fail construction rather than riding as a file or as text.
BOOST_AUTO_TEST_CASE(interpreter_rejects_audio_and_video_parts) {
    llm::responses::ResponsesInterpreter interpreter(
        std::make_shared<const FixtureDialect>());
    model_io::ModelEndpoint endpoint;
    endpoint.base_url = "https://example.invalid";
    endpoint.request_path = "/responses";

    for (const auto modality :
         {model_io::Modality::Audio, model_io::Modality::Video}) {
        model_io::AgentInputState state;
        auto& message = state.turns.emplace_back().user_input;
        message.type = model_io::MessageItemType::UserInput;
        auto& part = message.content.emplace_back();
        part.type = model_io::ContentType::ExternalRef;
        part.raw = "https://example.invalid/attachment";
        part.modality = modality;

        BOOST_CHECK_THROW(
            interpreter.build_request(
                state, endpoint, nlohmann::json{{"model", "fixture-model"}}),
            HttpRequestException);
    }
}

// The other half of the matrix: a kind this dialect has is still not a carrier
// for every representation, and the mismatch fails construction.
BOOST_AUTO_TEST_CASE(interpreter_rejects_an_encoding_the_kind_cannot_carry) {
    llm::responses::ResponsesInterpreter interpreter(
        std::make_shared<const FixtureDialect>());
    model_io::ModelEndpoint endpoint;
    endpoint.base_url = "https://example.invalid";
    endpoint.request_path = "/responses";
    const auto request = [&](model_io::ContentType type, model_io::Modality modality) {
        model_io::AgentInputState state;
        auto& part = state.turns.emplace_back().user_input.content.emplace_back();
        part.type = type;
        part.raw = "cGF5bG9hZA==";
        part.modality = modality;
        return interpreter.build_request(
            state, endpoint, nlohmann::json{{"model", "fixture-model"}});
    };

    // input_image takes a URL; base64 would need a media type we do not have.
    BOOST_CHECK_THROW(request(model_io::ContentType::Binary,
                              model_io::Modality::Image), HttpRequestException);
    // input_text would deliver the base64 encoding as the message.
    BOOST_CHECK_THROW(request(model_io::ContentType::Binary,
                              model_io::Modality::Text), HttpRequestException);
    // input_file takes a reference or base64 data, not a plain string.
    BOOST_CHECK_THROW(request(model_io::ContentType::Text,
                              model_io::Modality::Document), HttpRequestException);
    // Every documented pair still builds.
    BOOST_CHECK_NO_THROW(request(model_io::ContentType::Text,
                                 model_io::Modality::Text));
    BOOST_CHECK_NO_THROW(request(model_io::ContentType::ExternalRef,
                                 model_io::Modality::Text));
    BOOST_CHECK_NO_THROW(request(model_io::ContentType::ExternalRef,
                                 model_io::Modality::Image));
    BOOST_CHECK_NO_THROW(request(model_io::ContentType::ExternalRef,
                                 model_io::Modality::Document));
    BOOST_CHECK_NO_THROW(request(model_io::ContentType::Binary,
                                 model_io::Modality::Document));
}

// extras is caller-supplied data, not an instruction: whatever it claims, the
// provider part kind comes from the modality, and fields the chosen kind does
// not define are not forwarded.
BOOST_AUTO_TEST_CASE(interpreter_lets_neither_extras_type_nor_extra_fields_relabel_a_part) {
    llm::responses::ResponsesInterpreter interpreter(
        std::make_shared<const FixtureDialect>());
    model_io::ModelEndpoint endpoint;
    endpoint.base_url = "https://example.invalid";
    endpoint.request_path = "/responses";

    model_io::AgentInputState state;
    auto& message = state.turns.emplace_back().user_input;
    message.type = model_io::MessageItemType::UserInput;
    // A text part claiming to be a file.
    auto& text = message.content.emplace_back();
    text.type = model_io::ContentType::Text;
    text.raw = "describe this";
    text.modality = model_io::Modality::Text;
    text.extras = nlohmann::json{{"type", "input_file"}, {"file_data", "smuggled"}};
    // An image claiming to be a file.
    auto& image = message.content.emplace_back();
    image.type = model_io::ContentType::ExternalRef;
    image.raw = "https://example.invalid/cat.png";
    image.modality = model_io::Modality::Image;
    image.extras = nlohmann::json{{"type", "input_file"}, {"detail", "low"}};
    // A file claiming to be an image.
    auto& document = message.content.emplace_back();
    document.type = model_io::ContentType::Binary;
    document.raw = "ZmFrZS1wZGY=";
    document.modality = model_io::Modality::Document;
    document.extras = nlohmann::json{
        {"type", "input_image"}, {"filename", "sample.pdf"}};

    const auto body = nlohmann::json::parse(interpreter.build_request(
        state, endpoint, nlohmann::json{{"model", "fixture-model"}}).body());
    const auto& parts = body["input"][0]["content"];
    BOOST_REQUIRE_EQUAL(parts.size(), 3u);

    BOOST_CHECK_EQUAL(parts[0]["type"], "input_text");
    BOOST_CHECK_EQUAL(parts[0]["text"], "describe this");
    BOOST_CHECK(!parts[0].contains("file_data"));

    BOOST_CHECK_EQUAL(parts[1]["type"], "input_image");
    BOOST_CHECK_EQUAL(parts[1]["image_url"], "https://example.invalid/cat.png");
    BOOST_CHECK_EQUAL(parts[1]["detail"], "low");

    BOOST_CHECK_EQUAL(parts[2]["type"], "input_file");
    BOOST_CHECK_EQUAL(parts[2]["file_data"], "ZmFrZS1wZGY=");
    BOOST_CHECK_EQUAL(parts[2]["filename"], "sample.pdf");
}

// The matrix belongs to the POSITION. An image or a document is fine in the
// input list, and impossible in a synthesized assistant message, which this
// adapter emits as `output_text` — passing the input check and then replayed as
// the characters of a URL is the silent category change the label prevents.
BOOST_AUTO_TEST_CASE(interpreter_validates_assistant_content_as_a_text_position) {
    llm::responses::ResponsesInterpreter interpreter(
        std::make_shared<const FixtureDialect>());
    model_io::ModelEndpoint endpoint;
    endpoint.base_url = "https://example.invalid";
    endpoint.request_path = "/responses";
    const auto request = [&](model_io::Modality modality,
                             model_io::ContentType type) {
        model_io::AgentInputState state;
        auto& turn = state.turns.emplace_back();
        auto& user = turn.user_input.content.emplace_back();
        user.type = model_io::ContentType::Text;
        user.raw = "describe this";
        user.modality = model_io::Modality::Text;
        auto& step = turn.agent_loop_step.emplace_back();
        step.model_response.type = model_io::MessageItemType::ModelResponse;
        auto& part = step.model_response.content.emplace_back();
        part.type = type;
        part.raw = "https://example.invalid/answer";
        part.modality = modality;
        return interpreter.build_request(
            state, endpoint, nlohmann::json{{"model", "fixture-model"}});
    };

    BOOST_CHECK_THROW(request(model_io::Modality::Image,
                              model_io::ContentType::ExternalRef),
                      HttpRequestException);
    BOOST_CHECK_THROW(request(model_io::Modality::Document,
                              model_io::ContentType::ExternalRef),
                      HttpRequestException);
    // The very same pairs build in the user position.
    BOOST_CHECK_NO_THROW(request(model_io::Modality::Text,
                                 model_io::ContentType::Text));
    // A captured assistant message is re-emitted verbatim, so its Content is
    // not what reaches the provider and its label places no demand on it.
    {
        model_io::AgentInputState state;
        auto& turn = state.turns.emplace_back();
        auto& step = turn.agent_loop_step.emplace_back();
        step.model_response.type = model_io::MessageItemType::ModelResponse;
        step.model_response.extras = nlohmann::json{{"output_items",
            nlohmann::json::array({nlohmann::json{
                {"type", "message"}, {"role", "assistant"},
                {"content", nlohmann::json::array({nlohmann::json{
                    {"type", "output_text"}, {"text", "captured"}}})}}})}};
        auto& part = step.model_response.content.emplace_back();
        part.type = model_io::ContentType::ExternalRef;
        part.raw = "https://example.invalid/answer.png";
        part.modality = model_io::Modality::Image;
        BOOST_CHECK_NO_THROW(interpreter.build_request(
            state, endpoint, nlohmann::json{{"model", "fixture-model"}}));
    }
}

// Tool output goes out through the same input array as user content, so the
// input matrix applies there, and the captured fast path skips it.
BOOST_AUTO_TEST_CASE(interpreter_validates_tool_results_as_an_input_position) {
    llm::responses::ResponsesInterpreter interpreter(
        std::make_shared<const FixtureDialect>());
    model_io::ModelEndpoint endpoint;
    endpoint.base_url = "https://example.invalid";
    endpoint.request_path = "/responses";

    model_io::AgentInputState state;
    auto& turn = state.turns.emplace_back();
    auto& step = turn.agent_loop_step.emplace_back();
    step.model_response.type = model_io::MessageItemType::ModelResponse;
    model_io::MessageItem result;
    result.type = model_io::MessageItemType::InvokeReturn;
    auto& part = result.content.emplace_back();
    part.type = model_io::ContentType::ExternalRef;
    part.raw = "https://example.invalid/screenshot.png";
    part.modality = model_io::Modality::Image;
    step.invoke_returns = std::vector<model_io::MessageItem>{result};

    // The turn's user message is emitted first (empty here), so find the tool
    // result by its wire type rather than by position.
    const auto find = [](const nlohmann::json& body, const char* type) -> const nlohmann::json& {
        for (const auto& item : body["input"]) {
            if (item.value("type", std::string()) == type) return item;
        }
        throw std::runtime_error(std::string("no ") + type + " in the input list");
    };
    const auto body = nlohmann::json::parse(interpreter.build_request(
        state, endpoint, nlohmann::json{{"model", "fixture-model"}}).body());
    const auto& output = find(body, "function_call_output")["output"];
    BOOST_REQUIRE(output.is_array());
    BOOST_CHECK_EQUAL(output[0]["type"], "input_image");

    // The same part with a text representation that cannot survive the array.
    step.invoke_returns->front().content.front().type = model_io::ContentType::Text;
    BOOST_CHECK_THROW(interpreter.build_request(
        state, endpoint, nlohmann::json{{"model", "fixture-model"}}),
        HttpRequestException);
}

// A field this dialect never maps is not subject to provider capabilities.
BOOST_AUTO_TEST_CASE(interpreter_does_not_validate_unmapped_action_status) {
    llm::responses::ResponsesInterpreter interpreter(
        std::make_shared<const FixtureDialect>());
    model_io::ModelEndpoint endpoint;
    endpoint.base_url = "https://example.invalid";
    endpoint.request_path = "/responses";

    model_io::AgentInputState state;
    auto& step = state.turns.emplace_back().agent_loop_step.emplace_back();
    step.model_response.type = model_io::MessageItemType::ModelResponse;
    auto& content = step.model_response.content.emplace_back();
    content.type = model_io::ContentType::Text;
    content.raw = "the answer";
    content.modality = model_io::Modality::Text;
    auto& status = step.model_response.action_status.emplace();
    status.type = model_io::ContentType::ExternalRef;
    status.raw = "https://example.invalid/clip.mp4";
    status.modality = model_io::Modality::Video;

    const auto body = nlohmann::json::parse(interpreter.build_request(
        state, endpoint, nlohmann::json{{"model", "fixture-model"}}).body());
    const nlohmann::json* assistant = nullptr;
    for (const auto& item : body["input"]) {
        if (item.value("type", std::string()) == "message"
            && item.value("role", std::string()) == "assistant") {
            assistant = &item;
        }
    }
    BOOST_REQUIRE(assistant != nullptr);
    BOOST_REQUIRE_EQUAL((*assistant)["content"].size(), 1u);
    BOOST_CHECK_EQUAL((*assistant)["content"][0]["type"], "output_text");
    // The unmapped field added no provider part of its own.
    BOOST_CHECK_EQUAL(body["input"].size(), 2u);
}

// Reasoning is synthesized as a summary_text unless captured items are replayed
// verbatim, in which case the Content's own label is not emitted at all.
BOOST_AUTO_TEST_CASE(interpreter_validates_reasoning_only_when_it_is_synthesized) {
    llm::responses::ResponsesInterpreter interpreter(
        std::make_shared<const FixtureDialect>());
    model_io::ModelEndpoint endpoint;
    endpoint.base_url = "https://example.invalid";
    endpoint.request_path = "/responses";
    const auto state_with = [](model_io::Content reasoning) {
        model_io::AgentInputState state;
        auto& step = state.turns.emplace_back().agent_loop_step.emplace_back();
        step.model_response.type = model_io::MessageItemType::ModelResponse;
        auto& content = step.model_response.content.emplace_back();
        content.type = model_io::ContentType::Text;
        content.raw = "the answer";
        step.model_response.reasoning = std::move(reasoning);
        return state;
    };

    model_io::Content image;
    image.type = model_io::ContentType::ExternalRef;
    image.raw = "https://example.invalid/thought.png";
    image.modality = model_io::Modality::Image;
    BOOST_CHECK_THROW(interpreter.build_request(
        state_with(image), endpoint, nlohmann::json{{"model", "fixture-model"}}),
        HttpRequestException);

    image.extras = nlohmann::json{{"items", nlohmann::json::array({
        nlohmann::json{{"type", "reasoning"}, {"id", "rs_1"}}})}};
    BOOST_CHECK_NO_THROW(interpreter.build_request(
        state_with(image), endpoint, nlohmann::json{{"model", "fixture-model"}}));
}

BOOST_AUTO_TEST_CASE(converse_runs_llm_model_to_terminal_response_fallback) {
    const nlohmann::json terminal = {
        {"type", "response.completed"},
        {"response", {
            {"id", "resp_fixture"},
            {"status", "completed"},
            {"usage", {
                {"input_tokens", 7},
                {"output_tokens", 3},
                {"input_tokens_details", {{"cached_tokens", 2}}},
            }},
            {"output", nlohmann::json::array({{
                {"id", "msg_fixture"},
                {"type", "message"},
                {"role", "assistant"},
                {"status", "completed"},
                {"content", nlohmann::json::array({{
                    {"type", "output_text"}, {"text", "hello back"},
                    {"annotations", nlohmann::json::array()},
                }})},
            }})},
        }},
    };
    const std::string stream =
        "event: response.completed\ndata: " + terminal.dump() + "\n\n";
    loopback::OneShotServer server([&](asio::ip::tcp::socket& socket) {
        loopback::serve_fixed_response(socket, http::status::ok, stream);
    });
    const auto port = server.wait_listening();

    asio::io_context io;
    FixtureModel model(io.get_executor(), nlohmann::json{
        {"model", "fixture-model"},
        {"retry", {{"max_attempts", 1}}},
        {"endpoint", {
            {"base_url", "http://127.0.0.1:" + std::to_string(port)},
            {"request_path", "/responses"},
        }},
    });
    BOOST_REQUIRE(model.build());

    std::optional<model_io::MessageItem> result;
    std::exception_ptr failure;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            result = co_await model.converse(one_user_turn());
        } catch (...) {
            failure = std::current_exception();
        }
    }, asio::detached);
    io.run();
    server.join();

    if (failure) std::rethrow_exception(failure);
    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(result->content.at(0).raw, "hello back");
    BOOST_REQUIRE(result->cost);
    BOOST_CHECK_EQUAL(result->cost->prompt, 7u);
    BOOST_CHECK_EQUAL(result->cost->generated, 3u);
    BOOST_CHECK_EQUAL(result->cost->cache_hit, 2u);
    BOOST_REQUIRE(result->extras);
    BOOST_CHECK_EQUAL((*result->extras)["response_id"], "resp_fixture");
}

// retry.max_attempts counts RETRIES only: 0 disables retrying, so one
// recoverable truncation is the whole exchange — the transport failure
// propagates and the server saw exactly one connection.
BOOST_AUTO_TEST_CASE(converse_honours_zero_retry_budget) {
    TruncatingServer server(2);   // would accept a retry; none may come

    asio::io_context io;
    FixtureModel model(io.get_executor(), nlohmann::json{
        {"model", "fixture-model"},
        {"retry", {{"max_attempts", 0}}},
        {"endpoint", {
            {"base_url", "http://127.0.0.1:" + std::to_string(server.wait_listening())},
            {"request_path", "/responses"},
        }},
    });
    BOOST_REQUIRE(model.build());

    std::exception_ptr failure;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            co_await model.converse(one_user_turn());
        } catch (...) {
            failure = std::current_exception();
        }
    }, asio::detached);
    io.run();
    server.join();

    BOOST_REQUIRE(failure);
    try {
        std::rethrow_exception(failure);
    } catch (const HttpRequestException& e) {
        BOOST_CHECK(e.stage() == HttpRequestException::Stage::Read);
    } catch (...) {
        BOOST_FAIL("expected an HttpRequestException out of the truncation");
    }
    BOOST_CHECK_EQUAL(server.connections(), 1);   // initial exchange only
}

// ---------------------------------------------------------------------------
// set_generation + provider_info on the responses adapter (the contract's
// shared core is covered in test_models.cpp; here it is the adapter's
// derivation + the native-envelope passthrough that matter)
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(set_generation_layers_and_the_envelope_rides_verbatim) {
    asio::io_context io;
    FixtureModel model(io.get_executor(), nlohmann::json{
        {"provider", "fixture"},
        {"model", "fixture-model"},
        {"temperature", 0.2},
        {"endpoint", {{"base_url", "http://127.0.0.1:8123"}}},  // never dialed
    });
    BOOST_REQUIRE(model.build());
    BOOST_CHECK_EQUAL(model.generation()["temperature"], 0.2);

    model.set_generation(llm::GenerationPreset{
        .model = std::nullopt, .effort = llm::ReasoningEffort::Low,
    });
    BOOST_CHECK_EQUAL(model.generation()["reasoning"]["effort"], "low");

    // The Responses API carries the envelope natively: no top-level
    // translation, build_request passes it through verbatim.
    llm::responses::ResponsesInterpreter interpreter(
        std::make_shared<const FixtureDialect>());
    model_io::AgentInputState state;
    const auto request = interpreter.build_request(
        state, model.endpoint(), model.generation());
    const auto body = nlohmann::json::parse(request.body());
    BOOST_CHECK_EQUAL(body["reasoning"]["effort"], "low");
    BOOST_CHECK_EQUAL(body["model"], "fixture-model");

    // The idempotent second build() must not reset the knobs.
    BOOST_CHECK(model.build());
    BOOST_CHECK_EQUAL(model.generation()["reasoning"]["effort"], "low");
}

BOOST_AUTO_TEST_CASE(provider_info_normalises_the_catalogue_envelope) {
    const std::string catalogue = nlohmann::json{
        {"object", "list"},
        {"data", nlohmann::json::array({
            {{"id", "gpt-fixture-mini"}, {"object", "model"},
             {"owned_by", "fixture"}},
        })},
    }.dump();
    loopback::OneShotServer server([&](asio::ip::tcp::socket& socket) {
        loopback::serve_fixed_response(socket, http::status::ok, catalogue);
    });
    const auto port = server.wait_listening();

    asio::io_context io;
    FixtureModel model(io.get_executor(), nlohmann::json{
        {"model", "fixture-model"},
        {"endpoint", {
            {"base_url", "http://127.0.0.1:" + std::to_string(port)},
            {"request_path", "/responses"},
        }},
    });
    BOOST_REQUIRE(model.build());

    std::optional<nlohmann::json> result;
    std::exception_ptr failure;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            result = co_await model.provider_info();
        } catch (...) {
            failure = std::current_exception();
        }
    }, asio::detached);
    io.run();
    server.join();

    if (failure) std::rethrow_exception(failure);
    BOOST_REQUIRE(result);
    BOOST_REQUIRE(result->is_array());
    BOOST_REQUIRE_EQUAL(result->size(), 1u);
    BOOST_CHECK_EQUAL((*result)[0]["id"], "gpt-fixture-mini");
}
