#define BOOST_TEST_MODULE ModalityAssistTools
#include <boost/test/unit_test.hpp>

#include "tools/intrinsic/modality_assist/toolset.hpp"
#include "tools/intrinsic/modality_assist/tools.hpp"
#include "tools/invoke_exception.hpp"
#include "tools/registry.hpp"
#include "fileio/base64.hpp"
#include "fileio/read_prefix.hpp"
#include "llm/compat/chat_completions/interpreter.hpp"
#include "llm/compat/responses/interpreter.hpp"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_future.hpp>
#include <fstream>
#include <sys/stat.h>

namespace {
namespace asio = boost::asio;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using tools::intrinsic::ModalityAssistTool;
using tools::intrinsic::ModalityAssistToolSet;

/** Records each isolated request and completes asynchronously without network IO. */
struct Model : llm::LLMModel {
    explicit Model(asio::any_io_executor executor) : LLMModel(executor, {})
    {
        reply.type = model_io::MessageItemType::ModelResponse;
        reply.role = "assistant";
        reply.content.push_back({model_io::ContentType::Text, "A red pixel."});
        reply.reasoning = model_io::Content{model_io::ContentType::Text, "private reasoning"};
    }
    llm::LLMModelType model_type() const noexcept override { return type; }
    asio::awaitable<model_io::MessageItem> converse(model_io::AgentInputState state) override
    {
        inputs.push_back(std::move(state));
        asio::steady_timer timer(co_await asio::this_coro::executor, std::chrono::milliseconds(20));
        co_await timer.async_wait(asio::use_awaitable);
        if (fail) throw std::runtime_error("test provider failure");
        co_return reply;
    }
    llm::LLMModelType type = llm::LLMModelType::Conversation;
    std::vector<model_io::AgentInputState> inputs;
    model_io::MessageItem reply;
    bool fail = false;
};

struct Fixture {
    asio::io_context io;
    std::shared_ptr<Model> model = std::make_shared<Model>(io.get_executor());
    tools::ToolRegistry registry;
    fs::path root;

    Fixture()
    {
        auto pattern = (fs::temp_directory_path() / "simplex-modality-XXXXXX").string();
        const auto directory = ::mkdtemp(pattern.data());
        if (!directory) throw std::runtime_error("cannot create modality test directory");
        root = directory;
        registry.add(std::make_shared<ModalityAssistToolSet>(model));
    }
    ~Fixture()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    model_io::InvokeReturn call(Json arguments)
    {
        model_io::InvokeQuery query;
        query.name = "modality_assist";
        query.id = "vision-1";
        query.arguments = std::move(arguments);
        io.restart();
        auto result = asio::co_spawn(io,
            registry.execute({query}, io.get_executor()), asio::use_future);
        io.run();
        auto records = result.get();
        BOOST_REQUIRE(records.size() == 1u);
        return std::move(records.front());
    }
    Json arguments() const
    {
        return {{"path", MODALITY_IMAGE_FIXTURE}, {"request", "What color is the pixel?"}};
    }
};
} // namespace

BOOST_FIXTURE_TEST_CASE(one_exchange_defaults_and_image_bytes_are_preserved, Fixture)
{
    const auto result = call(arguments());
    BOOST_REQUIRE(!tools::is_error(result));
    BOOST_REQUIRE(model->inputs.size() == 1u);
    const auto& input = model->inputs.front();
    BOOST_REQUIRE(input.turns.size() == 1u);
    BOOST_TEST(input.tools.empty());
    BOOST_CHECK(!input.loop);
    BOOST_TEST(input.turns.front().agent_loop_step.empty());
    const auto& user = input.turns.front().user_input;
    BOOST_TEST(user.role == "user");
    BOOST_REQUIRE(user.content.size() == 2u);
    BOOST_TEST(user.content[0].raw == "What color is the pixel?");
    const auto& image = user.content[1];
    BOOST_CHECK(image.modality == model_io::Modality::Image);
    BOOST_CHECK(image.type == model_io::ContentType::ExternalRef);
    BOOST_REQUIRE(image.raw.starts_with("data:image/png;base64,"));
    BOOST_TEST(fileio::base64_decode(image.raw.substr(image.raw.find(',') + 1))
        == fileio::read_prefix(MODALITY_IMAGE_FIXTURE, ModalityAssistTool::kMaxFileBytes));
    BOOST_TEST(input.system_prompt.render().markdown.find(ModalityAssistTool::kDefaultSystemPrompt)
        != std::string::npos);
    BOOST_TEST(result.output.raw.find("A red pixel.") != std::string::npos);
    BOOST_TEST(result.output.raw.find("private reasoning") == std::string::npos);
    BOOST_TEST(result.output.raw.find("base64,") == std::string::npos);
    BOOST_CHECK(result.output.type == model_io::ContentType::Text);
    BOOST_CHECK(result.query.type == model_io::InvokeType::ReadOnly);
    BOOST_CHECK(result.query.security == model_io::InvokeSecurity::Trusted);

    ModalityAssistTool tool(model);
    const auto& schema = tool.get_details().argument_schema;
    BOOST_TEST(schema.at("properties").size() == 4u);
    BOOST_TEST(schema.at("required") == Json::array({"request", "path"}));
    for (const auto& [name, property] : schema.at("properties").items()) {
        if (property.contains("default")) {
            BOOST_TEST(result.query.arguments.at(name) == property.at("default"));
        }
    }
    model_io::PromptTemplate prompt;
    BOOST_TEST(registry.inject_skills(prompt) == 1u);
}

BOOST_FIXTURE_TEST_CASE(data_url_reaches_both_protocols_as_an_image, Fixture)
{
    BOOST_REQUIRE(!tools::is_error(call(arguments())));
    const auto& input = model->inputs.front();
    const auto& url = input.turns.front().user_input.content[1].raw;
    model_io::ModelEndpoint endpoint;
    endpoint.base_url = "https://model.example";
    endpoint.auth.scheme = model_io::AuthScheme::None;
    Json generation = {{"model", "vision-fixture"}};
    llm::chat_completions::ChatCompletionsInterpreter chat;
    const auto chat_body = Json::parse(chat.build_request(input, endpoint, generation).body());
    BOOST_TEST(chat_body["messages"].back()["content"][1]["image_url"]["url"] == url);
    llm::responses::ResponsesInterpreter responses;
    const auto response_body = Json::parse(responses.build_request(input, endpoint, generation).body());
    BOOST_TEST(response_body["input"].back()["content"][1]["image_url"] == url);
    BOOST_TEST(response_body["input"].back()["content"][1]["type"] == "input_image");
}

BOOST_FIXTURE_TEST_CASE(custom_prompts_and_successive_calls_have_no_shared_history, Fixture)
{
    auto args = arguments();
    args["system_prompt"] = "Extract visible text only.";
    args["request"] = "Read the labels.";
    model->reply.content.push_back({model_io::ContentType::Text, "Second paragraph."});
    const auto result = call(args);
    BOOST_REQUIRE(!tools::is_error(result));
    BOOST_TEST(result.output.raw.find("A red pixel.\nSecond paragraph.") != std::string::npos);
    BOOST_REQUIRE(!tools::is_error(call(arguments())));
    BOOST_REQUIRE(model->inputs.size() == 2u);
    BOOST_TEST(model->inputs[0].system_prompt.render().markdown.find("Extract visible text only.")
        != std::string::npos);
    BOOST_TEST(model->inputs[1].system_prompt.render().markdown.find("Extract visible text only.")
        == std::string::npos);
    BOOST_TEST(model->inputs[1].turns.size() == 1u);
    BOOST_TEST(model->inputs[1].turns[0].agent_loop_step.empty());
}

BOOST_FIXTURE_TEST_CASE(extension_checks_are_case_insensitive_and_only_advisory, Fixture)
{
    for (const auto& [extension, mime] : std::vector<std::pair<std::string, std::string>>{
        {".PNG", "image/png"}, {".JpG", "image/jpeg"}, {".jpeg", "image/jpeg"},
        {".gif", "image/gif"}, {".webp", "image/webp"}
    }) {
        const auto path = root / ("input" + extension);
        std::ofstream(path, std::ios::binary) << "suffix hint only";
        auto args = arguments();
        args["path"] = path.string();
        BOOST_REQUIRE(!tools::is_error(call(args)));
        BOOST_TEST(model->inputs.back().turns.front().user_input.content[1].raw.starts_with(
            "data:" + mime + ";base64,"));
    }
    const auto link = root / "linked.png";
    fs::create_symlink(MODALITY_IMAGE_FIXTURE, link);
    auto args = arguments();
    args["path"] = link.string();
    BOOST_REQUIRE(!tools::is_error(call(args)));
}

BOOST_FIXTURE_TEST_CASE(invalid_arguments_and_file_failures_do_not_call_the_model, Fixture)
{
    for (const auto& [key, value] : std::vector<std::pair<std::string, Json>>{
        {"request", ""}, {"request", nullptr}, {"request", 42},
        {"path", ""}, {"path", nullptr}, {"path", "file.txt"},
        {"path", std::string("bad\0.png", 8)},
        {"extra_modality", "audio"}, {"extra_modality", 1},
        {"system_prompt", ""}, {"system_prompt", false}
    }) {
        auto args = arguments();
        args[key] = value;
        BOOST_TEST(tools::is_error(call(args)));
    }
    const auto directory = root / "directory.png";
    fs::create_directory(directory);
    const auto pipe = root / "pipe.png";
    BOOST_REQUIRE(::mkfifo(pipe.c_str(), 0600) == 0);
    const auto empty = root / "empty.png";
    std::ofstream(empty).close();
    const auto large = root / "large.png";
    std::ofstream(large).close();
    fs::resize_file(large, ModalityAssistTool::kMaxFileBytes + 1);
    for (const auto& path : {directory, pipe, empty, large, root / "missing.png"}) {
        auto args = arguments();
        args["path"] = path.string();
        BOOST_TEST(tools::is_error(call(args)));
    }
    BOOST_TEST(model->inputs.empty());
}

BOOST_FIXTURE_TEST_CASE(provider_failures_and_invalid_replies_are_not_retried_by_the_tool, Fixture)
{
    model->fail = true;
    auto result = call(arguments());
    BOOST_TEST(tools::is_error(result));
    BOOST_TEST(result.output.raw.find("test provider failure") != std::string::npos);
    model->fail = false;
    model->reply.content.clear();
    BOOST_TEST(tools::is_error(call(arguments())));
    model->reply.content.push_back({model_io::ContentType::Binary, "AA=="});
    BOOST_TEST(tools::is_error(call(arguments())));
    model->reply.content = {{model_io::ContentType::Text, "description"}};
    model->reply.invokes = std::vector<model_io::InvokeQuery>{{}};
    BOOST_TEST(tools::is_error(call(arguments())));
    BOOST_TEST(model->inputs.size() == 4u);
}

BOOST_FIXTURE_TEST_CASE(model_is_owned_and_bad_injection_is_rejected, Fixture)
{
    BOOST_CHECK_THROW((void)ModalityAssistTool(nullptr), std::invalid_argument);
    model->type = llm::LLMModelType::Embedding;
    BOOST_CHECK_THROW((void)ModalityAssistTool(model), std::invalid_argument);
    model->type = llm::LLMModelType::Conversation;
    std::weak_ptr<Model> weak = model;
    model.reset();
    BOOST_TEST(!weak.expired());
    BOOST_REQUIRE(!tools::is_error(call(arguments())));
}

BOOST_FIXTURE_TEST_CASE(an_in_flight_exchange_finishes_despite_coroutine_cancellation, Fixture)
{
    ModalityAssistTool tool(model);
    model_io::InvokeQuery query;
    query.arguments = arguments();
    tool.ensure_arguments(query);
    asio::cancellation_signal cancel;
    auto result = asio::co_spawn(io, tool.invoke(query),
        asio::bind_cancellation_slot(cancel.slot(), asio::use_future));
    asio::steady_timer timer(io, std::chrono::milliseconds(5));
    timer.async_wait([&](boost::system::error_code error) {
        if (!error) cancel.emit(asio::cancellation_type::all);
    });
    io.run();
    BOOST_TEST(result.get().raw.find("A red pixel.") != std::string::npos);
    BOOST_TEST(model->inputs.size() == 1u);
}
