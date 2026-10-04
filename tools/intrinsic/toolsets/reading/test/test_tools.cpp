#define BOOST_TEST_MODULE ReadingTools
#include <boost/test/unit_test.hpp>

#include "tools/intrinsic/reading/toolset.hpp"
#include "tools/intrinsic/reading/tools.hpp"
#include "tools/intrinsic/reading/schemas.hpp"
#include "tools/registry.hpp"
#include "tools/invoke_exception.hpp"
#include "tools/intrinsic/tool_declaration.hpp"
#include "llm/compat/chat_completions/interpreter.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <limits>
#include <iostream>
#include <sstream>
#include <sys/stat.h>

namespace {
using Json = nlohmann::json;
namespace asio = boost::asio;

/** Restore an authoritative schema override even when an assertion throws. */
struct SchemaOverride {
    std::optional<std::string> previous;
    explicit SchemaOverride(const std::filesystem::path& path)
    {
        if (const auto old = std::getenv("SIMPLEX_READING_SCHEMA_DIR")) previous = old;
        BOOST_REQUIRE(::setenv("SIMPLEX_READING_SCHEMA_DIR", path.c_str(), 1) == 0);
    }
    ~SchemaOverride()
    {
        if (previous) ::setenv("SIMPLEX_READING_SCHEMA_DIR", previous->c_str(), 1);
        else ::unsetenv("SIMPLEX_READING_SCHEMA_DIR");
    }
};

/** Capture synchronous initialization diagnostics without changing log policy. */
struct ErrorCapture {
    std::ostringstream text;
    std::streambuf* previous = std::cerr.rdbuf(text.rdbuf());
    ~ErrorCapture() { std::cerr.rdbuf(previous); }
};

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
    std::filesystem::path schema(Json config, bool omit = false)
    {
        const auto original = tools::intrinsic::load_tool_declaration(
            tools::intrinsic::reading::schema_directory() / "read_text.yaml");
        const auto directory = root / "schemas";
        std::filesystem::create_directories(directory);
        Json declaration = {
            {"name", original.name}, {"description", original.description},
            {"argument_schema", original.argument_schema},
            // Deliberately contradictory documentation-only fields. Config
            // must not make these override the trusted/read-only C++ policy.
            {"type", "serial_write"}, {"security", "require_confirm"}
        };
        if (!omit) declaration["config"] = std::move(config);
        std::ofstream out(directory / "read_text.yaml");
        out << declaration;
        BOOST_REQUIRE(out.good());
        return directory;
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

BOOST_FIXTURE_TEST_CASE(dense_newlines_are_bounded_before_indexed_rendering, Fixture)
{
    const auto path = write(std::string(16 * 1024 * 1024, '\n'));
    const auto indexed = call({{"path", path}, {"format", "byte_range"},
                               {"count", std::numeric_limits<std::uint64_t>::max()}});
    BOOST_REQUIRE(!tools::is_error(indexed));
    contains(indexed, "[[total_lines]]: 16777217");
    contains(indexed, "[[lines_read]]: 16777217");
    contains(indexed, "[[output_truncated]]: true");
    contains(indexed, "[       0:       1] | ");
    BOOST_TEST(indexed.output.raw.size() < 70000u);

    const auto hex = call({{"path", path}, {"mode", "bytes"},
                           {"format", "hex_escaped"},
                           {"count", std::numeric_limits<std::uint64_t>::max()}});
    BOOST_REQUIRE(!tools::is_error(hex));
    contains(hex, "[[total_lines]]: 16777217");
    contains(hex, "[[output_truncated]]: true");
    contains(hex, "\\x0A\\x0A");
    BOOST_TEST(hex.output.raw.size() < 70000u);
}

BOOST_FIXTURE_TEST_CASE(malformed_continuation_run_crosses_display_boundary, Fixture)
{
    const auto path = write(std::string(70000, '\x80'));
    const auto result = call({{"path", path}, {"mode", "bytes"},
                              {"count", 70000}});
    BOOST_REQUIRE(!tools::is_error(result));
    contains(result, "[[display_replaced]]: true");
    contains(result, "[[output_truncated]]: true");
    contains(result, "File may not be UTF-8 text");
    contains(result, "\xef\xbf\xbd\xef\xbf\xbd");
    BOOST_TEST(result.output.raw.size() > 65000u);
    BOOST_TEST(result.output.raw.size() < 70000u);
    BOOST_CHECK_NO_THROW(Json(result.output.raw).dump());
}

BOOST_FIXTURE_TEST_CASE(missing_schema_override_disables_the_tool_and_skill, Fixture)
{
    SchemaOverride override(root / "missing");
    tools::intrinsic::ReadTextTool tool;
    BOOST_TEST(tool.get_details().name.empty());
    tools::intrinsic::ReadingToolSet set;
    BOOST_TEST(!set.skill().has_value());
}

BOOST_FIXTURE_TEST_CASE(configured_limits_are_owned_per_instance_and_hidden_from_models, Fixture)
{
    const auto source_skill = tools::intrinsic::reading::schema_directory() / "skill.yaml";
    const auto directory = schema({{"max_file_bytes", 64}, {"max_output_bytes", 7}});
    std::filesystem::copy_file(source_skill, directory / "skill.yaml");
    SchemaOverride override(directory);
    auto first = std::make_shared<tools::intrinsic::ReadingToolSet>();
    BOOST_REQUIRE(first->get_tools().size() == 1u);
    registry.clear();
    registry.add(first);
    const auto path = write("abcdefghijklmnop");
    const auto first_result = call({{"path", path}, {"mode", "bytes"}, {"count", 100}});
    BOOST_REQUIRE(!tools::is_error(first_result));
    contains(first_result, "Output clipped to 7 bytes");
    contains(first_result, "text (truncated, first 7 bytes):\nabcdefg");
    BOOST_CHECK(first_result.query.type == model_io::InvokeType::ReadOnly);
    BOOST_CHECK(first_result.query.security == model_io::InvokeSecurity::Trusted);

    // Neither advertised definitions, injected skill, arguments, nor persisted
    // state should carry the host settings. Also check the actual provider wire.
    model_io::AgentInputState state;
    state.tools = registry.get_tools();
    BOOST_TEST(registry.inject_skills(state.system_prompt) == 1u);
    auto& turn = state.turns.emplace_back();
    turn.user_input.role = "user";
    turn.user_input.content.emplace_back().raw = "Read the file.";
    auto& step = turn.agent_loop_step.emplace_back();
    step.model_response.type = model_io::MessageItemType::ModelResponse;
    step.model_response.role = "assistant";
    step.model_response.invokes = {first_result.query};
    model_io::MessageItem result_message;
    result_message.type = model_io::MessageItemType::InvokeReturn;
    result_message.role = "tool";
    result_message.content = {first_result.output};
    result_message.invoke_return = first_result;
    step.invoke_returns = {result_message};
    const auto serialized = Json(state).dump();
    llm::chat_completions::ChatCompletionsInterpreter interpreter;
    model_io::ModelEndpoint endpoint;
    endpoint.base_url = "http://127.0.0.1";
    const auto body = Json::parse(interpreter.build_request(
        state, endpoint, Json{{"model", "fixture"}}).body());
    for (const auto key : {"max_file_bytes", "max_output_bytes"}) {
        BOOST_TEST(serialized.find(key) == std::string::npos);
        BOOST_TEST(body.dump().find(key) == std::string::npos);
        BOOST_TEST(first_result.query.arguments.count(key) == 0u);
    }
    BOOST_TEST(!body["tools"][0]["function"].contains("config"));
    BOOST_TEST(!Json(state.tools[0]).contains("config"));

    // An argument cannot change operator settings, even when it is injected
    // directly into a query rather than selected through the advertised schema.
    const auto attempted_override = call({
        {"path", path}, {"mode", "bytes"}, {"count", 100},
        {"max_output_bytes", 1000}, {"config", {{"max_output_bytes", 1000}}}
    });
    BOOST_REQUIRE(!tools::is_error(attempted_override));
    contains(attempted_override, "Output clipped to 7 bytes");

    // Rewrite that same YAML. No companion config.yaml and no recompilation.
    schema({{"max_file_bytes", 12}, {"max_output_bytes", 11}});
    auto second = std::make_shared<tools::intrinsic::ReadingToolSet>();
    BOOST_REQUIRE(second->get_tools().size() == 1u);
    const auto unchanged = call({{"path", path}, {"mode", "bytes"}, {"count", 100}});
    BOOST_REQUIRE(!tools::is_error(unchanged));
    contains(unchanged, "Output clipped to 7 bytes");
    registry.clear();
    registry.add(second);
    BOOST_TEST(tools::is_error(call({{"path", path}}))); // 16 bytes exceeds 12.
    const auto changed = call({{"path", write("abcdefghijkl")}, {"count", 100}});
    BOOST_REQUIRE(!tools::is_error(changed));
    contains(changed, "Output clipped to 11 bytes");
    contains(changed, "text (truncated, first 11 bytes):\nabcdefghijk");
}

BOOST_FIXTURE_TEST_CASE(configuration_omission_and_empty_mapping_keep_read_defaults, Fixture)
{
    for (const bool omit : {false, true}) {
        const auto directory = schema(Json::object(), omit);
        SchemaOverride override(directory);
        registry.clear();
        registry.add(std::make_shared<tools::intrinsic::ReadingToolSet>());
        const auto result = call({{"path", write(std::string(65537, 'a'))}});
        BOOST_REQUIRE(!tools::is_error(result));
        contains(result, "Output clipped to 65536 bytes");
        BOOST_TEST(tools::is_error(call({{"path", write(std::string(
            tools::intrinsic::ReadTextTool::kMaxFileBytes + 1, 'a'))}})));
    }
}

BOOST_FIXTURE_TEST_CASE(invalid_configuration_names_file_field_and_isolates_registration, Fixture)
{
    const std::vector<std::pair<Json, std::string>> invalid = {
        {nullptr, "/config"}, {Json::array(), "/config"},
        {Json::array({"max_output_bytes", "distinctive-secret"}), "/config"},
        {Json{{"max_file_bytes", 0}}, "/config/max_file_bytes"},
        {Json{{"max_file_bytes", -1}}, "/config/max_file_bytes"},
        {Json{{"max_file_bytes", 1.5}}, "/config/max_file_bytes"},
        {Json{{"max_file_bytes", true}}, "/config/max_file_bytes"},
        {Json{{"max_file_bytes", "distinctive-secret"}}, "/config/max_file_bytes"},
        {Json{{"max_file_bytes", nullptr}}, "/config/max_file_bytes"},
        {Json{{"max_file_bytes", 1073741825}}, "/config/max_file_bytes"},
        {Json{{"max_file_bytes", std::numeric_limits<std::uint64_t>::max()}}, "/config/max_file_bytes"},
        {Json{{"max_output_bytes", 0}}, "/config/max_output_bytes"},
        {Json{{"max_output_bytes", 16777217}}, "/config/max_output_bytes"},
        {Json{{"max_output_bytes", Json::object()}}, "/config/max_output_bytes"},
        {Json{{"max_file_bytes", 3}, {"unknown", "distinctive-secret"}}, "/config/unknown"}
    };
    for (const auto& [config, field] : invalid) {
        const auto directory = schema(config);
        SchemaOverride override(directory);
        ErrorCapture errors;
        auto invalid_set = std::make_shared<tools::intrinsic::ReadingToolSet>();
        BOOST_TEST(invalid_set->get_tools().empty());
        BOOST_TEST(errors.text.str().find((directory / "read_text.yaml").string()) != std::string::npos);
        BOOST_TEST(errors.text.str().find(field) != std::string::npos);
        BOOST_TEST(errors.text.str().find("distinctive-secret") == std::string::npos);
        // The fixture's already-constructed healthy set is unaffected, even
        // after an empty/degraded set is added to the same real registry.
        registry.add(invalid_set);
        BOOST_TEST(registry.get_tools().size() == 1u);
        BOOST_TEST(!tools::is_error(call({{"path", write("still available")}})));
    }
}

/** Also run from an installed tree after changing its YAML byte limits. */
BOOST_FIXTURE_TEST_CASE(selected_yaml_configuration_controls_read_limits, Fixture)
{
    const auto declaration = tools::intrinsic::load_tool_declaration(
        tools::intrinsic::reading::schema_directory() / "read_text.yaml");
    const auto file_limit = declaration.config.value("max_file_bytes",
        tools::intrinsic::ReadTextTool::kMaxFileBytes);
    const auto output_limit = declaration.config.value("max_output_bytes",
        tools::intrinsic::ReadTextTool::kMaxOutputBytes);
    BOOST_TEST(tools::is_error(call({{"path", write(std::string(file_limit + 1, 'a'))}})));
    if (output_limit < file_limit) {
        const auto result = call({{"path", write(std::string(output_limit + 1, 'a'))}});
        BOOST_REQUIRE(!tools::is_error(result));
        contains(result, "Output clipped to " + std::to_string(output_limit) + " bytes");
        contains(result, "text (truncated, first " + std::to_string(output_limit) + " bytes)");
    }
}

BOOST_FIXTURE_TEST_CASE(valid_configuration_boundaries_cover_line_byte_and_expanded_display, Fixture)
{
    for (const auto& [file_bytes, output_bytes] :
         std::vector<std::pair<std::size_t, std::size_t>>{
             {1, 1}, {32, 4},
             {tools::intrinsic::ReadTextTool::kMaxConfiguredFileBytes,
              tools::intrinsic::ReadTextTool::kMaxConfiguredOutputBytes}}) {
        const auto directory = schema({
            {"max_file_bytes", file_bytes}, {"max_output_bytes", output_bytes}
        });
        SchemaOverride override(directory);
        registry.clear();
        auto set = std::make_shared<tools::intrinsic::ReadingToolSet>();
        BOOST_REQUIRE(set->get_tools().size() == 1u);
        registry.add(set);
        const auto path = write(file_bytes == 1 ? "a" : "A\xe2\x82\xac" "B");
        for (const auto& [mode, format] :
             std::vector<std::pair<std::string, std::string>>{
                 {"lines", "plain"}, {"lines", "line_index"},
                 {"lines", "byte_range"}, {"bytes", "plain"}, {"bytes", "hex_escaped"}}) {
            const auto result = call({{"path", path}, {"mode", mode}, {"format", format}});
            BOOST_REQUIRE(!tools::is_error(result));
            BOOST_CHECK_NO_THROW(Json(result.output.raw).dump());
            if (output_bytes == 4) {
                contains(result, "Output clipped to 4 bytes");
                if (mode == "bytes" && format == "hex_escaped") {
                    contains(result, "text (truncated, first 4 bytes):\n\\x41");
                } else if (format == "plain") {
                    contains(result, "text (truncated, first 4 bytes):\nA\xe2\x82\xac");
                }
            }
        }
    }
}

BOOST_FIXTURE_TEST_CASE(nonfinite_yaml_configuration_keeps_file_field_and_mark_diagnostics, Fixture)
{
    const auto directory = root / "nonfinite-schemas";
    std::filesystem::create_directories(directory);
    const auto file = directory / "read_text.yaml";
    for (const std::string scalar : {".nan", ".inf"}) {
        {
            // Raw YAML is essential: JSON cannot represent these scalar values.
            std::ofstream out(file);
            out << "name: read_text\ndescription: Read a regular file.\n"
                   "argument_schema: {type: object}\nconfig:\n"
                   "  max_file_bytes: " << scalar << "\n"
                   "  private_note: distinctive-secret\n";
            BOOST_REQUIRE(out.good());
        }
        SchemaOverride override(directory);
        ErrorCapture errors;
        tools::intrinsic::ReadTextTool tool;
        BOOST_TEST(tool.get_details().name.empty());
        BOOST_TEST(!tool.build());
        auto set = std::make_shared<tools::intrinsic::ReadingToolSet>();
        BOOST_TEST(set->get_tools().empty());
        model_io::InvokeQuery query;
        query.name = "read_text";
        BOOST_TEST(set->dispatch(query) == nullptr);
        const auto diagnostic = errors.text.str();
        BOOST_TEST(diagnostic.find(file.string()) != std::string::npos);
        BOOST_TEST(diagnostic.find("/config/max_file_bytes") != std::string::npos);
        BOOST_TEST(diagnostic.find("(line 4, column 18)") != std::string::npos);
        BOOST_TEST(diagnostic.find("non-finite number") != std::string::npos);
        BOOST_TEST(diagnostic.find("distinctive-secret") == std::string::npos);
        BOOST_TEST(diagnostic.find("private_note") == std::string::npos);
    }
}
