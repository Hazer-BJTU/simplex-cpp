#define BOOST_TEST_MODULE EditingTools
#include <boost/test/unit_test.hpp>

#include "tools/intrinsic/editing/toolset.hpp"
#include "tools/intrinsic/editing/tools.hpp"
#include "tools/intrinsic/editing/schemas.hpp"
#include "tools/registry.hpp"
#include "tools/security_check.hpp"
#include "tools/invoke_exception.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <unistd.h>

namespace {
namespace asio = boost::asio;
using Json = nlohmann::json;

struct Fixture {
    std::filesystem::path root;
    asio::io_context io;
    tools::ToolRegistry registry;

    Fixture()
    {
        auto pattern = (std::filesystem::temp_directory_path() / "simplex-edit-tool-XXXXXX").string();
        const auto directory = ::mkdtemp(pattern.data());
        if (!directory) throw std::runtime_error("cannot create edit tool test directory");
        root = directory;
        registry.add(std::make_shared<tools::intrinsic::EditingToolSet>());
    }

    ~Fixture()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    std::string write(const std::string& text)
    {
        const auto path = root / "source";
        std::ofstream output(path, std::ios::binary);
        output << text;
        output.close();
        return path.string();
    }

    std::string read() const
    {
        std::ifstream input(root / "source", std::ios::binary);
        return {std::istreambuf_iterator<char>(input), {}};
    }

    model_io::InvokeReturn call(Json arguments, bool approve = true)
    {
        auto confirmation = eventbus::default_async_bus().subscribe<tools::InvokeConfirmEvent>(
            [approve](tools::InvokeConfirmEvent event)
                -> asio::awaitable<tools::InvokeConfirmEvent> {
                event.decision = approve ? tools::ConfirmDecision::Approved
                                         : tools::ConfirmDecision::Denied;
                co_return event;
            });
        model_io::InvokeQuery query;
        query.id = "edit-1";
        query.name = "str_replace_edit";
        query.arguments = std::move(arguments);
        auto future = asio::co_spawn(io,
            registry.execute({query}, io.get_executor()), asio::use_future);
        io.restart();
        io.run();
        auto records = future.get();
        confirmation.disconnect();
        BOOST_REQUIRE_EQUAL(records.size(), 1u);
        return std::move(records.front());
    }
};

void contains(const model_io::InvokeReturn& result, std::string_view text)
{
    BOOST_TEST(result.output.raw.find(text) != std::string::npos);
}
}

BOOST_FIXTURE_TEST_CASE(schema_registration_confirmation_and_aligned_preview, Fixture)
{
    tools::intrinsic::StrReplaceEditTool tool;
    const auto schema = tool.get_details().argument_schema;
    BOOST_TEST(tool.get_details().name == "str_replace_edit");
    BOOST_TEST(schema.at("required") == Json::array({"path", "old_text", "new_text"}));
    BOOST_TEST(schema.at("properties").at("context_lines").at("default") == 3);
    BOOST_TEST(schema.at("properties").at("context_lines").at("minimum") == 0);
    BOOST_TEST(schema.at("properties").at("context_lines").at("maximum") == 20);
    model_io::PromptTemplate prompt;
    BOOST_TEST(registry.inject_skills(prompt) == 1u);
    tools::intrinsic::EditingToolSet set;
    BOOST_REQUIRE(set.skill().has_value());
    BOOST_TEST(set.skill()->text.find("str_replace_edit") != std::string::npos);

    const auto path = write("one\nold\nend\n");
    const auto result = call({{"path", path}, {"old_text", "old"}, {"new_text", "new"},
                              {"context_lines", 1}});
    BOOST_REQUIRE(!tools::is_error(result));
    BOOST_CHECK(result.query.type == model_io::InvokeType::SerialWrite);
    BOOST_CHECK(result.query.security == model_io::InvokeSecurity::RequireConfirm);
    contains(result, "[[status]]: modified");
    contains(result, "  0 | one\n- 1 | old\n  2 | end");
    contains(result, "  0 | one\n+ 1 | new\n  2 | end");
    BOOST_TEST(read() == "one\nnew\nend\n");
    const auto upper_boundary = call({{"path", path}, {"old_text", "new"},
                                      {"new_text", "final"}, {"context_lines", 20}});
    BOOST_REQUIRE(!tools::is_error(upper_boundary));
    contains(upper_boundary, "+ 1 | final");
    BOOST_TEST(read() == "one\nfinal\nend\n");
}

BOOST_AUTO_TEST_CASE(installed_declarations_take_precedence_when_present)
{
    const auto installed = tools::intrinsic::editing::schema_directory();
    BOOST_TEST(std::filesystem::exists(installed / "str_replace_edit.yaml"));
    BOOST_TEST(std::filesystem::exists(installed / "skill.yaml"));
    tools::intrinsic::EditingToolSet set;
    BOOST_TEST(set.skill().has_value());
}

BOOST_FIXTURE_TEST_CASE(denial_invalid_arguments_and_ambiguous_match_do_not_write, Fixture)
{
    const auto path = write("old\nold\n");
    BOOST_TEST(tools::is_error(call({{"path", path}, {"old_text", "old"},
                                     {"new_text", "new"}}, false)));
    BOOST_TEST(read() == "old\nold\n");
    BOOST_TEST(tools::is_error(call({{"path", path}, {"old_text", "old"},
                                     {"new_text", "new"}})));
    BOOST_TEST(read() == "old\nold\n");
    BOOST_TEST(tools::is_error(call({{"path", path}, {"old_text", ""},
                                     {"new_text", "new"}})));
    BOOST_TEST(tools::is_error(call({{"path", path}, {"old_text", "old"}})));
    BOOST_TEST(tools::is_error(call({{"path", path}, {"old_text", "old"},
                                     {"new_text", ""}, {"context_lines", 21}})));
    BOOST_TEST(read() == "old\nold\n");
}
