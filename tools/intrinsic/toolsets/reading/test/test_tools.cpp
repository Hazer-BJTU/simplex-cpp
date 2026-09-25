#define BOOST_TEST_MODULE ReadingTools
#include <boost/test/unit_test.hpp>

#include "tools/intrinsic/reading/toolset.hpp"
#include "tools/intrinsic/reading/tools.hpp"
#include "tools/intrinsic/reading/schemas.hpp"
#include "tools/registry.hpp"
#include "tools/invoke_exception.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>

namespace {
using Json = nlohmann::json;
namespace asio = boost::asio;

/** Real registry and isolated file tree, with no confirmation subscriber. */
struct Fixture {
    std::filesystem::path root;
    asio::io_context io;
    tools::ToolRegistry registry;

    Fixture()
    {
        auto pattern = (std::filesystem::temp_directory_path() / "simplex-reading-XXXXXX").string();
        const auto directory = ::mkdtemp(pattern.data());
        if (!directory) {
            throw std::runtime_error("cannot create reading test directory");
        }
        root = directory;
        registry.add(std::make_shared<tools::intrinsic::ReadingToolSet>());
    }
    ~Fixture()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::string write(const std::string& bytes)
    {
        const auto path = root / "input";
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        BOOST_REQUIRE(output.good());
        return path.string();
    }
    model_io::InvokeReturn call(Json arguments)
    {
        model_io::InvokeQuery query;
        query.name = "read_text";
        query.id = "read-1";
        query.arguments = std::move(arguments);
        auto future = asio::co_spawn(io,
            registry.execute({query}, io.get_executor()), asio::use_future);
        io.restart();
        io.run();
        auto records = future.get();
        BOOST_REQUIRE_EQUAL(records.size(), 1u);
        return std::move(records.front());
    }
};

/// Assert model-facing metadata or literal output without depending on field order.
void contains(const model_io::InvokeReturn& result, std::string_view expected)
{
    BOOST_TEST(result.output.raw.find(expected) != std::string::npos,
               "missing " << expected << " in " << result.output.raw);
}
}

BOOST_FIXTURE_TEST_CASE(defaults_schema_skill_and_attributes_match, Fixture)
{
    tools::intrinsic::ReadTextTool tool;
    const auto schema = tool.get_details().argument_schema;
    BOOST_TEST(tool.get_details().name == "read_text");
    BOOST_TEST(schema.at("properties").size() == 5u);
    BOOST_TEST(schema.at("required") == Json::array({"path"}));
    BOOST_TEST(schema.at("properties").at("path").at("minLength") == 1);
    BOOST_TEST(schema.at("properties").at("mode").at("enum") == Json::array({"lines", "bytes"}));
    BOOST_TEST(schema.at("properties").at("format").at("enum") ==
               Json::array({"plain", "line_index", "byte_range", "hex_escaped"}));
    for (const auto key : {"start", "count"}) {
        BOOST_TEST(schema.at("properties").at(key).at("type") == "integer");
        BOOST_TEST(schema.at("properties").at(key).at("minimum") == 0);
    }
    const auto record = call({{"path", write("hello\n")}});
    BOOST_REQUIRE(!tools::is_error(record));
    BOOST_CHECK(record.query.type == model_io::InvokeType::ReadOnly);
    BOOST_CHECK(record.query.security == model_io::InvokeSecurity::Trusted);
    for (const auto& [name, property] : schema.at("properties").items()) {
        if (property.contains("default")) {
            BOOST_TEST(record.query.arguments.at(name) == property.at("default"));
        }
    }
    contains(record, "[[lines_read]]: 2");
    contains(record, "[[reached_end]]: true");
    contains(record, "hello\n");
    model_io::PromptTemplate prompt;
    BOOST_TEST(registry.inject_skills(prompt) == 1u);
    tools::intrinsic::ReadingToolSet set;
    BOOST_REQUIRE(set.skill().has_value());
    BOOST_TEST(set.skill()->text.find("read_text") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(installed_declarations_take_precedence_when_present)
{
    const auto executable = std::filesystem::read_symlink("/proc/self/exe");
    const auto installed = executable.parent_path() / "schemas" / "reading";
    if (const auto override = std::getenv("SIMPLEX_READING_SCHEMA_DIR");
        override != nullptr && *override != '\0') {
        BOOST_CHECK(tools::intrinsic::reading::schema_directory() == override);
    } else if (std::filesystem::is_directory(installed)) {
        BOOST_CHECK(tools::intrinsic::reading::schema_directory() == installed);
    } else {
        BOOST_TEST(std::filesystem::is_regular_file(
            tools::intrinsic::reading::schema_directory() / "read_text.yaml"));
    }
}

BOOST_FIXTURE_TEST_CASE(all_read_modes_preserve_expected_selection, Fixture)
{
    const auto path = write("abc\r\ndefgh\nZ");
    for (const auto& [format, expected] : std::vector<std::pair<std::string, std::string>>{
             {"plain", "abc\r\ndefgh\nZ"},
             {"line_index", "0 | abc\n1 | defgh\n2 | Z"},
             {"byte_range", "[ 0: 5] | abc\n[ 5:11] | defgh\n[11:12] | Z"}}) {
        const auto result = call({{"path", path}, {"format", format}});
        BOOST_REQUIRE(!tools::is_error(result));
        contains(result, expected);
        contains(result, "[[total_bytes]]: 12");
        contains(result, "[[total_lines]]: 3");
    }
    const auto hex = call({{"path", path}, {"mode", "bytes"}, {"start", 3},
                           {"count", 2}, {"format", "hex_escaped"}});
    BOOST_REQUIRE(!tools::is_error(hex));
    contains(hex, "\\x0D\\x0A");
    contains(hex, "[[total_bytes]]: 12");
    contains(hex, "[[total_lines]]: 3");
    const auto plain = call({{"path", path}, {"mode", "bytes"}, {"start", 5}, {"count", 5}});
    BOOST_REQUIRE(!tools::is_error(plain));
    contains(plain, "defgh");
    const auto empty = call({{"path", path}, {"count", 0}});
    BOOST_REQUIRE(!tools::is_error(empty));
    contains(empty, "[[lines_read]]: 0");
    contains(empty, "text: (empty)");
}

BOOST_FIXTURE_TEST_CASE(invalid_arguments_and_file_failures_are_tool_errors, Fixture)
{
    const auto path = write("a");
    for (const Json arguments : std::vector<Json>{
             Json::object(), {{"path", ""}}, {{"path", 42}}, {{"path", std::string("a\0b", 3)}},
             {{"path", path}, {"mode", "other"}}, {{"path", path}, {"format", "hex_escaped"}},
             {{"path", path}, {"mode", "bytes"}, {"format", "line_index"}},
             {{"path", path}, {"mode", "bytes"}, {"format", "byte_range"}},
             {{"path", path}, {"format", "unknown"}}, {{"path", path}, {"count", -1}},
             {{"path", path}, {"start", 0.5}}, {{"path", path}, {"count", "1"}},
             {{"path", path}, {"start", 5}}, {{"path", (root / "missing").string()}},
             {{"path", root.string()}}}) {
        BOOST_TEST(tools::is_error(call(arguments)));
    }
    BOOST_REQUIRE(::mkfifo((root / "pipe").c_str(), 0600) == 0);
    BOOST_TEST(tools::is_error(call({{"path", (root / "pipe").string()}})));
    auto null_defaults = call({{"path", path}, {"start", nullptr}, {"count", nullptr},
                               {"format", nullptr}, {"mode", nullptr}});
    BOOST_REQUIRE(!tools::is_error(null_defaults));
    BOOST_TEST(null_defaults.query.arguments.at("count") == 200);
}

BOOST_FIXTURE_TEST_CASE(metadata_omits_selection_arguments_and_reports_whole_file_totals, Fixture)
{
    const auto path = write("a\r\nb\n");
    for (const std::string mode : {"lines", "bytes"}) {
        for (const auto count : {0, 1}) {
            const auto result = call({{"path", path}, {"mode", mode},
                                      {"start", 1}, {"count", count}});
            BOOST_REQUIRE(!tools::is_error(result));
            contains(result, "[[total_lines]]: 3");
            contains(result, "[[total_bytes]]: 5");
            const auto metadata = result.output.raw.substr(0, result.output.raw.find("\n\n"));
            for (const std::string field : {
                     "mode", "format", "start_byte", "end_byte", "start_line", "end_line"}) {
                BOOST_TEST(metadata.find("[[" + field + "]]:") == std::string::npos);
            }
        }
    }
    const auto empty = call({{"path", write("")}, {"mode", "bytes"}});
    BOOST_REQUIRE(!tools::is_error(empty));
    contains(empty, "[[total_lines]]: 1");
    contains(empty, "[[total_bytes]]: 0");
}

BOOST_FIXTURE_TEST_CASE(invalid_utf8_is_advisory_and_safe_for_model_transport, Fixture)
{
    const auto path = write(std::string("a\xff\0b", 4));
    const auto plain = call({{"path", path}});
    BOOST_REQUIRE(!tools::is_error(plain));
    contains(plain, "File may not be UTF-8 text");
    const auto hint_position = plain.output.raw.find("[[hints]]:");
    const auto content_position = plain.output.raw.find("\n\ntext (");
    BOOST_REQUIRE(hint_position != std::string::npos);
    BOOST_REQUIRE(content_position != std::string::npos);
    BOOST_TEST(hint_position < content_position);
    BOOST_TEST(plain.output.raw.find("File may not be UTF-8 text") < content_position);
    contains(plain, "[[display_replaced]]: true");
    BOOST_CHECK_NO_THROW(Json(plain.output.raw).dump());
    const auto hex = call({{"path", path}, {"mode", "bytes"}, {"format", "hex_escaped"}});
    BOOST_REQUIRE(!tools::is_error(hex));
    contains(hex, "\\x61\\xFF\\x00\\x62");
    contains(hex, "[[display_replaced]]: false");
    const auto split = call({{"path", write("\xe4\xb8\xad")}, {"mode", "bytes"},
                             {"start", 1}, {"count", 1}});
    BOOST_REQUIRE(!tools::is_error(split));
    contains(split, "[[display_replaced]]: true");
}

BOOST_FIXTURE_TEST_CASE(output_limits_are_visible_and_preserve_utf8_boundaries, Fixture)
{
    std::string bytes(65535, 'a');
    bytes += "\xe4\xb8\xad";
    const auto result = call({{"path", write(bytes)}});
    BOOST_REQUIRE(!tools::is_error(result));
    contains(result, "[[output_truncated]]: true");
    contains(result, "[[total_bytes]]: 65538");
    contains(result, "[[total_lines]]: 1");
    contains(result, "[[display_replaced]]: false");
    BOOST_CHECK_NO_THROW(Json(result.output.raw).dump());
    const auto oversize = write(std::string(tools::intrinsic::ReadTextTool::kMaxFileBytes + 1, 'a'));
    BOOST_TEST(tools::is_error(call({{"path", oversize}})));
}

BOOST_FIXTURE_TEST_CASE(missing_schema_override_disables_the_tool_and_skill, Fixture)
{
    // Restore the process environment even if construction or an assertion throws.
    struct Override {
        std::optional<std::string> previous;
        explicit Override(const std::string& path)
        {
            if (const auto old = std::getenv("SIMPLEX_READING_SCHEMA_DIR")) previous = old;
            ::setenv("SIMPLEX_READING_SCHEMA_DIR", path.c_str(), 1);
        }
        ~Override()
        {
            if (previous) ::setenv("SIMPLEX_READING_SCHEMA_DIR", previous->c_str(), 1);
            else ::unsetenv("SIMPLEX_READING_SCHEMA_DIR");
        }
    } override((root / "missing").string());
    tools::intrinsic::ReadTextTool tool;
    BOOST_TEST(tool.get_details().name.empty());
    tools::intrinsic::ReadingToolSet set;
    BOOST_TEST(!set.skill().has_value());
}
