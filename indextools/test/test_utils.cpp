#define BOOST_TEST_MODULE UtilsTests
#include <boost/test/unit_test.hpp>

#include "indextools/utils.hpp"
#include "indextools/schema.hpp"
#include "indextools/split.hpp"

#include <algorithm>

using namespace indextools;

// ============================================================================
// Helper: build a minimal valid locate_entity-style JSON entity dict
// ============================================================================

namespace {

nlohmann::json make_entity(const std::string& file,
                           const std::string& type,
                           const std::string& target,
                           const std::string& lines,
                           const std::vector<std::string>& content,
                           const std::vector<size_t>& line_numbers,
                           const nlohmann::json& children = nlohmann::json::array())
{
    nlohmann::json meta;
    meta["field_name"] = nlohmann::json::array({"File", "Type", "Target", "Lines"});
    meta["field_content"] = nlohmann::json::array({file, type, target, lines});

    nlohmann::json text;
    text["line_content"] = content;
    text["line_number"] = line_numbers;
    text["line_type"] = nlohmann::json::array();
    for (size_t i = 0; i < content.size(); ++i) {
        text["line_type"].push_back("base");
    }

    nlohmann::json entity;
    entity["meta"] = meta;
    entity["text"] = text;
    entity["sub_entity"] = children;
    return entity;
}

} // anonymous namespace

// ============================================================================
// Suite: JsonToReadableTextSuite — backward-compat wrapper
// ============================================================================

BOOST_AUTO_TEST_SUITE(JsonToReadableTextSuite)

BOOST_AUTO_TEST_CASE(empty_array_returns_empty)
{
    nlohmann::json input = nlohmann::json::array();
    std::string result = json_to_readable_text(input);
    BOOST_CHECK_EQUAL(result, "(empty)\n");
}

BOOST_AUTO_TEST_CASE(non_array_object_renders_as_unknown)
{
    // An empty object has no distinguishing schema keys → Unknown fallback.
    nlohmann::json input = nlohmann::json::object();
    std::string result = json_to_readable_text(input);
    BOOST_CHECK_EQUAL(result, "{}\n");
}

BOOST_AUTO_TEST_CASE(null_json_returns_null_response)
{
    nlohmann::json input;
    std::string result = json_to_readable_text(input);
    BOOST_CHECK_EQUAL(result, "(null response)\n");
}

BOOST_AUTO_TEST_CASE(single_entity_contains_expected_sections)
{
    auto entity = make_entity("/f.py", "function_definition", "f.py(module) > foo(function_definition)",
                              "[0, 1]", {"def foo():", "    pass"}, {0, 1});

    nlohmann::json input = nlohmann::json::array({entity});
    std::string result = json_to_readable_text(input);

    // Should contain the key labels (right-aligned in meta table)
    BOOST_CHECK(result.find("File") != std::string::npos);
    BOOST_CHECK(result.find("Type") != std::string::npos);
    BOOST_CHECK(result.find("Target") != std::string::npos);
    BOOST_CHECK(result.find("Lines") != std::string::npos);

    // Should contain the meta values
    BOOST_CHECK(result.find("/f.py") != std::string::npos);
    BOOST_CHECK(result.find("function_definition") != std::string::npos);

    // Should contain source content
    BOOST_CHECK(result.find("def foo():") != std::string::npos);
    BOOST_CHECK(result.find("pass") != std::string::npos);

    // Should contain "Match" label
    BOOST_CHECK(result.find("Match") != std::string::npos);

    // Should contain Source section
    BOOST_CHECK(result.find("Source") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(multiple_matches_have_numbered_labels)
{
    auto e1 = make_entity("/a.py", "function_definition", "a.py(module) > foo(function_definition)",
                           "[0, 1]", {"def foo():", "    pass"}, {0, 1});
    auto e2 = make_entity("/a.py", "class_method", "a.py(module) > Bar(class_definition) > foo(class_method)",
                           "[3, 4]", {"    def foo(self):", "        pass"}, {3, 4});

    nlohmann::json input = nlohmann::json::array({e1, e2});
    std::string result = json_to_readable_text(input);

    BOOST_CHECK(result.find("Match #1 of 2") != std::string::npos);
    BOOST_CHECK(result.find("Match #2 of 2") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(entity_with_children_shows_sub_entities_section)
{
    auto child = make_entity("/b.py", "class_method", "b.py(module) > Foo(class_definition) > bar(class_method)",
                              "[1, 2]", {"    def bar(self):", "        return 42"}, {1, 2});

    auto parent = make_entity("/b.py", "class_definition", "b.py(module) > Foo(class_definition)",
                               "[0, 3]", {"class Foo:"}, {0},
                               nlohmann::json::array({child}));

    nlohmann::json input = nlohmann::json::array({parent});
    std::string result = json_to_readable_text(input);

    BOOST_CHECK(result.find("Sub-entities (1)") != std::string::npos);
    BOOST_CHECK(result.find("Match.1") != std::string::npos);
    BOOST_CHECK(result.find("def bar(self):") != std::string::npos);
    BOOST_CHECK(result.find("return 42") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(deeply_nested_entities_are_fully_rendered)
{
    // class A → class B → def c → def d
    auto d = make_entity("/d.py", "function_definition", "... > d(function_definition)",
                          "[3, 4]", {"            def d():", "                pass"}, {3, 4});
    auto c = make_entity("/d.py", "class_method", "... > c(class_method)",
                          "[2, 4]", {"        def c(self):"}, {2},
                          nlohmann::json::array({d}));
    auto b = make_entity("/d.py", "class_definition", "... > B(class_definition)",
                          "[1, 4]", {"    class B:"}, {1},
                          nlohmann::json::array({c}));
    auto a = make_entity("/d.py", "class_definition", "... > A(class_definition)",
                          "[0, 4]", {"class A:"}, {0},
                          nlohmann::json::array({b}));

    nlohmann::json input = nlohmann::json::array({a});
    std::string result = json_to_readable_text(input);

    // All 4 levels should appear
    BOOST_CHECK(result.find("class A:") != std::string::npos);
    BOOST_CHECK(result.find("class B:") != std::string::npos);
    BOOST_CHECK(result.find("def c(self):") != std::string::npos);
    BOOST_CHECK(result.find("def d():") != std::string::npos);
    BOOST_CHECK(result.find("pass") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ascii_flag_maps_to_ai_readable_mode)
{
    auto entity = make_entity("/f.py", "function_definition", "f.py(module) > foo(function_definition)",
                              "[0, 1]", {"def foo():", "    pass"}, {0, 1});

    nlohmann::json input = nlohmann::json::array({entity});

    std::string human_result = json_to_readable_text(input, false);
    std::string ai_result    = json_to_readable_text(input, true);

    // AI mode must NOT contain Unicode box-drawing bytes
    // ═ = E2 95 90, ─ = E2 94 80, │ = E2 94 82
    BOOST_CHECK(ai_result.find("\xe2\x95\x90") == std::string::npos);  // ═
    BOOST_CHECK(ai_result.find("\xe2\x94\x80") == std::string::npos);  // ─
    BOOST_CHECK(ai_result.find("\xe2\x94\x82") == std::string::npos);  // │

    // AI mode uses bracket-delimited labels and pipe-separated meta
    BOOST_CHECK(ai_result.find("[Match]") != std::string::npos);
    BOOST_CHECK(ai_result.find("File: /f.py") != std::string::npos);
    BOOST_CHECK(ai_result.find("|") != std::string::npos);

    // Human mode should contain Unicode box-drawing bytes
    bool has_unicode_box = false;
    has_unicode_box |= (human_result.find("\xe2\x95\x90") != std::string::npos);  // ═
    has_unicode_box |= (human_result.find("\xe2\x94\x80") != std::string::npos);  // ─
    has_unicode_box |= (human_result.find("\xe2\x94\x82") != std::string::npos);  // │
    BOOST_CHECK(has_unicode_box);
}

BOOST_AUTO_TEST_CASE(leaf_entity_no_sub_entities_section)
{
    // A leaf entity (empty sub_entity) should NOT have a Sub-entities section
    auto entity = make_entity("/f.py", "function_definition", "f.py(module) > foo(function_definition)",
                              "[0, 1]", {"def foo():", "    pass"}, {0, 1});

    nlohmann::json input = nlohmann::json::array({entity});
    std::string result = json_to_readable_text(input);

    BOOST_CHECK(result.find("Sub-entities") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(line_type_annotations_displayed)
{
    auto entity = make_entity("/f.py", "function_definition", "f.py(module) > foo(function_definition)",
                              "[0, 3]", {"def foo():", "    x = 1", "    return x"}, {0, 1, 2});

    // Override line types to test annotations
    entity["text"]["line_type"] = nlohmann::json::array({"base", "add", "delete"});

    nlohmann::json input = nlohmann::json::array({entity});
    std::string result = json_to_readable_text(input);

    // 'base' line: blank prefix ' '
    // 'add' line: '+' prefix
    // 'delete' line: '-' prefix
    BOOST_CHECK(result.find("+ ") != std::string::npos);  // '+' followed by space (the prefix)
    BOOST_CHECK(result.find("- ") != std::string::npos);  // '-' followed by space
}

BOOST_AUTO_TEST_CASE(entity_missing_meta_still_renders)
{
    nlohmann::json entity;
    entity["text"]["line_content"] = nlohmann::json::array({"print(1)"});
    entity["text"]["line_number"]  = nlohmann::json::array({0});
    entity["text"]["line_type"]    = nlohmann::json::array({"base"});
    entity["sub_entity"] = nlohmann::json::array();

    nlohmann::json input = nlohmann::json::array({entity});
    std::string result = json_to_readable_text(input);

    // Should still show Match label and Source section
    BOOST_CHECK(result.find("Match") != std::string::npos);
    BOOST_CHECK(result.find("Source") != std::string::npos);
    BOOST_CHECK(result.find("print(1)") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(entity_missing_text_still_renders)
{
    nlohmann::json entity;
    entity["meta"]["field_name"] = nlohmann::json::array({"Key"});
    entity["meta"]["field_content"] = nlohmann::json::array({"Value"});
    entity["sub_entity"] = nlohmann::json::array();

    nlohmann::json input = nlohmann::json::array({entity});
    std::string result = json_to_readable_text(input);

    // Should still show Match label and meta
    BOOST_CHECK(result.find("Match") != std::string::npos);
    BOOST_CHECK(result.find("Key") != std::string::npos);
    BOOST_CHECK(result.find("Value") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(result_is_not_empty_for_valid_input)
{
    auto entity = make_entity("/f.py", "function_definition", "f.py(module) > foo(function_definition)",
                              "[0, 1]", {"def foo():", "    pass"}, {0, 1});

    nlohmann::json input = nlohmann::json::array({entity});
    std::string result = json_to_readable_text(input);

    BOOST_CHECK(!result.empty());
    BOOST_CHECK(result.size() > 50u);  // should be substantial output
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: RenderResponseSuite — schema-aware rendering
// ============================================================================

BOOST_AUTO_TEST_SUITE(RenderResponseSuite)

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(null_input)
{
    BOOST_CHECK_EQUAL(render_response(nlohmann::json()), "(null response)\n");
}

BOOST_AUTO_TEST_CASE(empty_array)
{
    BOOST_CHECK_EQUAL(render_response(nlohmann::json::array()), "(empty)\n");
}

BOOST_AUTO_TEST_CASE(non_object_non_array)
{
    BOOST_CHECK_EQUAL(render_response(nlohmann::json(42)), "(invalid response)\n");
}

BOOST_AUTO_TEST_CASE(unknown_object_fallback)
{
    // An object without any schema-distinguishing keys → JSON dump fallback.
    nlohmann::json unknown;
    unknown["custom_key"] = "custom_value";
    std::string result = render_response(unknown);
    BOOST_CHECK(result.find("custom_key") != std::string::npos);
    BOOST_CHECK(result.find("custom_value") != std::string::npos);
}

// ---------------------------------------------------------------------------
// DisplayBlock — human_readable
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(display_block_human_single)
{
    auto block = schema::text_block(
        schema::MetaBuilder()
            .field("File", "/app/main.cpp")
            .field("Type", "function_definition")
            .build(),
        schema::TextBody()
            .line("int main() {", 10, schema::line_type::base)
            .line("    return 0;", 11, schema::line_type::match)
            .build()
    );

    std::string result = render_response(block, RenderMode::human_readable);

    // Header
    BOOST_CHECK(result.find("Match") != std::string::npos);
    // Unicode double rule (═ = U+2550)
    BOOST_CHECK(result.find("\xe2\x95\x90") != std::string::npos);
    // Meta values
    BOOST_CHECK(result.find("/app/main.cpp") != std::string::npos);
    BOOST_CHECK(result.find("function_definition") != std::string::npos);
    // Source section
    BOOST_CHECK(result.find("Source") != std::string::npos);
    BOOST_CHECK(result.find("int main() {") != std::string::npos);
    // Match line has '*' prefix
    BOOST_CHECK(result.find("* ") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(display_block_human_multiple)
{
    auto b1 = schema::text_block(
        schema::MetaBuilder().field("File", "/a.cpp").build(),
        schema::TextBody().line("// a", 0).build()
    );
    auto b2 = schema::text_block(
        schema::MetaBuilder().field("File", "/b.cpp").build(),
        schema::TextBody().line("// b", 0).build()
    );

    std::string result = render_response(
        nlohmann::json::array({b1, b2}), RenderMode::human_readable);

    BOOST_CHECK(result.find("Match #1 of 2") != std::string::npos);
    BOOST_CHECK(result.find("Match #2 of 2") != std::string::npos);
    BOOST_CHECK(result.find("/a.cpp") != std::string::npos);
    BOOST_CHECK(result.find("/b.cpp") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(display_block_human_with_content_body)
{
    auto block = schema::content_block(
        schema::MetaBuilder().field("File", "/data.bin").build(),
        "raw binary content here"
    );

    std::string result = render_response(block, RenderMode::human_readable);

    BOOST_CHECK(result.find("Content") != std::string::npos);
    BOOST_CHECK(result.find("raw binary content here") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(display_block_human_with_sub_entities)
{
    auto child = schema::text_block(
        schema::MetaBuilder().field("Type", "class_method").build(),
        schema::TextBody().line("    void foo();", 5).build()
    );
    auto parent = schema::text_block(
        schema::MetaBuilder().field("Type", "class_definition").build(),
        schema::TextBody().line("class Bar {", 3).build()
    );
    // Attach sub_entity manually
    parent["sub_entity"] = nlohmann::json::array({child});

    std::string result = render_response(parent, RenderMode::human_readable);

    BOOST_CHECK(result.find("Sub-entities (1)") != std::string::npos);
    BOOST_CHECK(result.find("Match.1") != std::string::npos);
    BOOST_CHECK(result.find("class_method") != std::string::npos);
}

// ---------------------------------------------------------------------------
// DisplayBlock — ai_readable
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(display_block_ai_single)
{
    auto block = schema::text_block(
        schema::MetaBuilder()
            .field("File", "/app/main.cpp")
            .field("Lines", "[10, 11]")
            .build(),
        schema::TextBody()
            .line("int main() {", 10, schema::line_type::base)
            .line("    return 0;", 11, schema::line_type::match)
            .build()
    );

    std::string result = render_response(block, RenderMode::ai_readable);

    // Bracket label
    BOOST_CHECK(result.find("[Match]") != std::string::npos);
    // Inline meta
    BOOST_CHECK(result.find("File: /app/main.cpp") != std::string::npos);
    BOOST_CHECK(result.find("Lines: [10, 11]") != std::string::npos);
    BOOST_CHECK(result.find(" | ") != std::string::npos);
    // Source lines (indented with 2 spaces + prefix)
    BOOST_CHECK(result.find("  int main() {") != std::string::npos);
    BOOST_CHECK(result.find("*     return 0;") != std::string::npos);
    // Must NOT contain Unicode box-drawing
    BOOST_CHECK(result.find("\xe2\x95\x90") == std::string::npos);
    BOOST_CHECK(result.find("\xe2\x94\x80") == std::string::npos);
    BOOST_CHECK(result.find("\xe2\x94\x82") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(display_block_ai_multiple)
{
    auto b1 = schema::text_block(
        schema::MetaBuilder().field("File", "/a.cpp").build(),
        schema::TextBody().line("// a", 0).build()
    );
    auto b2 = schema::text_block(
        schema::MetaBuilder().field("File", "/b.cpp").build(),
        schema::TextBody().line("// b", 0).build()
    );

    std::string result = render_response(
        nlohmann::json::array({b1, b2}), RenderMode::ai_readable);

    BOOST_CHECK(result.find("[Match #1 of 2]") != std::string::npos);
    BOOST_CHECK(result.find("[Match #2 of 2]") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(display_block_ai_with_content_body)
{
    auto block = schema::content_block(
        schema::MetaBuilder().field("File", "/data.bin").build(),
        "raw binary content here"
    );

    std::string result = render_response(block, RenderMode::ai_readable);

    BOOST_CHECK(result.find("raw binary content here") != std::string::npos);
    BOOST_CHECK(result.find("File: /data.bin") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(display_block_ai_with_sub_entities)
{
    auto child = schema::text_block(
        schema::MetaBuilder().field("Type", "class_method").build(),
        schema::TextBody().line("    void foo();", 5).build()
    );
    auto parent = schema::text_block(
        schema::MetaBuilder().field("Type", "class_definition").build(),
        schema::TextBody().line("class Bar {", 3).build()
    );
    parent["sub_entity"] = nlohmann::json::array({child});

    std::string result = render_response(parent, RenderMode::ai_readable);

    BOOST_CHECK(result.find("[Sub: 1]") != std::string::npos);
    BOOST_CHECK(result.find("[Match.1]") != std::string::npos);
    BOOST_CHECK(result.find("class_method") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(display_block_ai_is_compact)
{
    // AI mode should be significantly shorter than human mode for equivalent input.
    auto block = schema::text_block(
        schema::MetaBuilder()
            .field("File", "/home/user/project/src/main.cpp")
            .field("Type", "function_definition")
            .field("Lines", "[100, 120]")
            .build(),
        schema::TextBody()
            .line("void processData(const Input& in, Output& out) {", 100)
            .line("    for (auto& item : in.items()) {", 101)
            .line("        out.push_back(transform(item));", 102)
            .line("    }", 103)
            .line("}", 104)
            .build()
    );

    std::string human = render_response(block, RenderMode::human_readable);
    std::string ai    = render_response(block, RenderMode::ai_readable);

    BOOST_CHECK(ai.size() < human.size());
    // AI mode should be at least 40% smaller
    BOOST_CHECK(ai.size() * 100 / human.size() < 60);
}

// ---------------------------------------------------------------------------
// ProcessReport
// ---------------------------------------------------------------------------

nlohmann::json make_process_report()
{
    nlohmann::json status;
    status["status"] = "finished";
    status["exit_code"] = 0;
    status["execution_milliseconds"] = 42;
    return schema::ProcessReport(1, "task #1", status)
        .stream("hello world\n", nullptr)
        .build();
}

BOOST_AUTO_TEST_CASE(process_report_human)
{
    auto proc = make_process_report();
    std::string result = render_response(proc, RenderMode::human_readable);

    BOOST_CHECK(result.find("Process #1") != std::string::npos);
    BOOST_CHECK(result.find("ID") != std::string::npos);
    BOOST_CHECK(result.find("finished") != std::string::npos);
    BOOST_CHECK(result.find("Exit Code") != std::string::npos);
    BOOST_CHECK(result.find("stdout") != std::string::npos);
    BOOST_CHECK(result.find("stderr") != std::string::npos);
    BOOST_CHECK(result.find("hello world") != std::string::npos);
    BOOST_CHECK(result.find("(empty)") != std::string::npos);  // stderr is null
}

BOOST_AUTO_TEST_CASE(process_report_ai)
{
    auto proc = make_process_report();
    std::string result = render_response(proc, RenderMode::ai_readable);

    BOOST_CHECK(result.find("[Process #1]") != std::string::npos);
    BOOST_CHECK(result.find("ID: 1") != std::string::npos);
    BOOST_CHECK(result.find("Status: finished") != std::string::npos);
    BOOST_CHECK(result.find("Exit Code: 0") != std::string::npos);
    BOOST_CHECK(result.find("Elapsed (ms): 42") != std::string::npos);
    BOOST_CHECK(result.find("stdout: hello world\n") != std::string::npos);
    BOOST_CHECK(result.find("stderr: (empty)") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(process_report_multiple)
{
    nlohmann::json s1, s2;
    s1["status"] = "running"; s1["exit_code"] = nullptr; s1["execution_milliseconds"] = 10;
    s2["status"] = "finished"; s2["exit_code"] = 0; s2["execution_milliseconds"] = 99;

    auto p1 = schema::ProcessReport(0, "worker-0", s1).build();
    auto p2 = schema::ProcessReport(1, "worker-1", s2).build();

    std::string result = render_response(
        nlohmann::json::array({p1, p2}), RenderMode::human_readable);

    BOOST_CHECK(result.find("Process #1 of 2") != std::string::npos);
    BOOST_CHECK(result.find("Process #2 of 2") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(process_report_list_status_no_streams)
{
    // list_status shape: meta only, no stdout/stderr keys
    nlohmann::json status;
    status["status"] = "running";
    status["exit_code"] = nullptr;
    status["execution_milliseconds"] = 5;
    auto proc = schema::ProcessReport(7, "background task", status).build();

    std::string result = render_response(proc, RenderMode::ai_readable);

    BOOST_CHECK(result.find("[Process #1]") != std::string::npos);
    BOOST_CHECK(result.find("ID: 7") != std::string::npos);
    // No stdout/stderr keys present — should still render cleanly
    BOOST_CHECK(result.find("stdout: (empty)") != std::string::npos);
}

// ---------------------------------------------------------------------------
// ErrorReport
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(error_report_human)
{
    auto err = schema::validation_error("search",
        schema::error_code::invalid_argument,
        "Parameter 'pattern' cannot be empty.").build();

    std::string result = render_response(err, RenderMode::human_readable);

    BOOST_CHECK(result.find("Error") != std::string::npos);
    BOOST_CHECK(result.find("Command") != std::string::npos);
    BOOST_CHECK(result.find("search") != std::string::npos);
    BOOST_CHECK(result.find("Phase") != std::string::npos);
    BOOST_CHECK(result.find("validation") != std::string::npos);
    BOOST_CHECK(result.find("Code") != std::string::npos);
    BOOST_CHECK(result.find("INVALID_ARGUMENT") != std::string::npos);
    BOOST_CHECK(result.find("Parameter 'pattern' cannot be empty.") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(error_report_ai)
{
    auto err = schema::execution_error("subprocess",
        schema::error_code::timeout,
        "Command timed out after 30 s.").build();

    std::string result = render_response(err, RenderMode::ai_readable);

    BOOST_CHECK(result.find("[Error]") != std::string::npos);
    BOOST_CHECK(result.find("Command: subprocess") != std::string::npos);
    BOOST_CHECK(result.find("Phase: execution") != std::string::npos);
    BOOST_CHECK(result.find("Code: TIMEOUT") != std::string::npos);
    BOOST_CHECK(result.find("Message: Command timed out after 30 s.") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(error_report_with_detail)
{
    auto err = schema::execution_error("subprocess",
        schema::error_code::subprocess,
        "Child process crashed.")
        .detail({{"pid", 1234}, {"signal", "SIGSEGV"}})
        .build();

    std::string result = render_response(err, RenderMode::ai_readable);

    // detail is present in the JSON but render_response doesn't render it
    // specially — it should still render the message and meta correctly
    BOOST_CHECK(result.find("[Error]") != std::string::npos);
    BOOST_CHECK(result.find("Message: Child process crashed.") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(error_report_multiple)
{
    auto e1 = schema::validation_error("edit", "INVALID_ARGUMENT", "Bad path.").build();
    auto e2 = schema::execution_error("edit", "IO_ERROR", "Write failed.").build();

    std::string result = render_response(
        nlohmann::json::array({e1, e2}), RenderMode::human_readable);

    // Both errors should appear
    BOOST_CHECK(result.find("Bad path.") != std::string::npos);
    BOOST_CHECK(result.find("Write failed.") != std::string::npos);
}

// ---------------------------------------------------------------------------
// ExternalRef
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(external_ref_human)
{
    auto ref = schema::ExternalRef("/tmp/plot.png", 524288,
        schema::content_type::image,
        schema::external_reason::binary_data)
        .message("Chart rendered to /tmp/plot.png")
        .build();

    std::string result = render_response(ref, RenderMode::human_readable);

    BOOST_CHECK(result.find("External Content") != std::string::npos);
    BOOST_CHECK(result.find("Path") != std::string::npos);
    BOOST_CHECK(result.find("/tmp/plot.png") != std::string::npos);
    BOOST_CHECK(result.find("Bytes") != std::string::npos);
    BOOST_CHECK(result.find("524288") != std::string::npos);
    BOOST_CHECK(result.find("Type") != std::string::npos);
    BOOST_CHECK(result.find("image") != std::string::npos);
    BOOST_CHECK(result.find("Reason") != std::string::npos);
    BOOST_CHECK(result.find("binary_data") != std::string::npos);
    BOOST_CHECK(result.find("Chart rendered to /tmp/plot.png") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(external_ref_ai)
{
    auto ref = schema::ExternalRef("/tmp/result.txt", 2'097'152,
        schema::content_type::text,
        schema::external_reason::size_limit).build();

    std::string result = render_response(ref, RenderMode::ai_readable);

    BOOST_CHECK(result.find("[External Content]") != std::string::npos);
    BOOST_CHECK(result.find("Path: /tmp/result.txt") != std::string::npos);
    BOOST_CHECK(result.find("Bytes: 2097152") != std::string::npos);
    BOOST_CHECK(result.find("Type: text") != std::string::npos);
    BOOST_CHECK(result.find("Reason: size_limit") != std::string::npos);
    BOOST_CHECK(result.find("Message: Content written to /tmp/result.txt") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(external_ref_default_message)
{
    // Default auto-generated message from constructor
    auto ref = schema::ExternalRef("/tmp/data.bin", 1024,
        schema::content_type::binary,
        schema::external_reason::size_limit).build();

    std::string result = render_response(ref, RenderMode::ai_readable);

    BOOST_CHECK(result.find("Content written to /tmp/data.bin") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Mixed / heterogeneous arrays
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(mixed_schema_types_in_array)
{
    // When the array contains different schema types, detection uses the first
    // item. All items are rendered with the same renderer.
    auto block = schema::text_block(
        schema::MetaBuilder().field("File", "/f.py").build(),
        schema::TextBody().line("x = 1", 0).build()
    );
    auto err = schema::validation_error("search", "NOT_FOUND", "No results.").build();

    // First item is a DisplayBlock → all rendered as DisplayBlock
    std::string result = render_response(
        nlohmann::json::array({block, err}), RenderMode::ai_readable);

    // The first item renders as a match
    BOOST_CHECK(result.find("[Match #1 of 2]") != std::string::npos);
    // The second item (ErrorReport) will be rendered with DisplayBlock renderer
    // since detection is based on the first item
    BOOST_CHECK(result.find("[Match #2 of 2]") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: SplitedStringSuite — SplitedString construction, access, and iteration
// ============================================================================
//
// The SplitedString API has been redesigned (dev/reconstruct_cpptools branch):
//
//   - Constructor takes only delimiter patterns (default: {"\n", "\r\n"}).
//   - load(std::string)  loads source in-memory and builds the chunk index.
//   - read(path)         loads source from a file.
//   - operator[]         returns chunk CONTENT only (no delimiter).
//   - get_delimiter(i)   returns the delimiter after chunk i.
//   - get_chunk_index()  maps byte offset → chunk index (renamed from chunk_idx).
//   - No append() —       call load() again to replace the source.
//   - No keep_delim / strip_crlf — content and delimiters are always separate.
//
// Content/delimiter model (ChunkIndex):
//   Each chunk stores (index_start, content_length, delimiter_length).
//   - index_start:   byte offset where the chunk's content begins.
//   - content_length: length of content (without delimiter).
//   - delimiter_length: length of the delimiter that follows this chunk
//     (0 for the final chunk with no trailing delimiter).
//
//   Reconstructed source for chunk i = content + delimiter

BOOST_AUTO_TEST_SUITE(SplitedStringSuite)

// ---- Construction and default state ----

BOOST_AUTO_TEST_CASE(default_construction)
{
    SplitedString ss;
    BOOST_CHECK(ss.source().empty());
    BOOST_CHECK_EQUAL(ss.size(), 0u);
}

BOOST_AUTO_TEST_CASE(default_construction_has_default_delimiters)
{
    // Default delimiters {"\n", "\r\n"} — loading a source with these
    // delimiters should split correctly.
    SplitedString ss;
    ss.load("a\nb\r\nc");
    BOOST_CHECK_EQUAL(ss.size(), 3u);
    BOOST_CHECK_EQUAL(ss[0], "a");
    BOOST_CHECK_EQUAL(ss[1], "b");
    BOOST_CHECK_EQUAL(ss[2], "c");
}

BOOST_AUTO_TEST_CASE(custom_delimiter_construction)
{
    // Custom delimiter: split on "|"
    SplitedString ss({"|"});
    ss.load("a|b|c");
    BOOST_CHECK_EQUAL(ss.size(), 3u);
    BOOST_CHECK_EQUAL(ss[0], "a");
    BOOST_CHECK_EQUAL(ss[1], "b");
    BOOST_CHECK_EQUAL(ss[2], "c");
}

BOOST_AUTO_TEST_CASE(multiple_custom_delimiters)
{
    // Multiple delimiter patterns — both "::" and ";;" split the source.
    SplitedString ss({"::", ";;"});
    ss.load("a::b;;c");
    BOOST_CHECK_EQUAL(ss.size(), 3u);
    BOOST_CHECK_EQUAL(ss[0], "a");
    BOOST_CHECK_EQUAL(ss[1], "b");
    BOOST_CHECK_EQUAL(ss[2], "c");
}

// ---- load() — basic splitting ----

BOOST_AUTO_TEST_CASE(load_simple_lf_source)
{
    SplitedString ss;
    ss.load("a\nb\nc");
    BOOST_CHECK_EQUAL(ss.size(), 3u);
    BOOST_CHECK_EQUAL(ss[0], "a");
    BOOST_CHECK_EQUAL(ss[1], "b");
    BOOST_CHECK_EQUAL(ss[2], "c");
    BOOST_CHECK_EQUAL(ss.source(), "a\nb\nc");
}

BOOST_AUTO_TEST_CASE(load_crlf_source)
{
    // Default delimiters {"\n", "\r\n"} handle Windows line endings.
    SplitedString ss;
    ss.load("a\r\nb\r\nc");
    BOOST_CHECK_EQUAL(ss.size(), 3u);
    BOOST_CHECK_EQUAL(ss[0], "a");
    BOOST_CHECK_EQUAL(ss[1], "b");
    BOOST_CHECK_EQUAL(ss[2], "c");
}

BOOST_AUTO_TEST_CASE(load_mixed_line_endings)
{
    // Mixed LF and CRLF in the same source.
    SplitedString ss;
    ss.load("a\nb\r\nc\nd");
    BOOST_CHECK_EQUAL(ss.size(), 4u);
    BOOST_CHECK_EQUAL(ss[0], "a");
    BOOST_CHECK_EQUAL(ss[1], "b");
    BOOST_CHECK_EQUAL(ss[2], "c");
    BOOST_CHECK_EQUAL(ss[3], "d");
}

BOOST_AUTO_TEST_CASE(load_empty_source)
{
    SplitedString ss;
    ss.load("");
    BOOST_CHECK_EQUAL(ss.size(), 1u);
    BOOST_CHECK(ss[0].empty());
}

BOOST_AUTO_TEST_CASE(load_trailing_delimiter)
{
    SplitedString ss;
    ss.load("a\n");
    BOOST_CHECK_EQUAL(ss.size(), 2u);
    BOOST_CHECK_EQUAL(ss[0], "a");
    BOOST_CHECK(ss[1].empty());  // trailing empty chunk after final delimiter
}

BOOST_AUTO_TEST_CASE(load_consecutive_delimiters)
{
    SplitedString ss;
    ss.load("a\n\nb");
    BOOST_CHECK_EQUAL(ss.size(), 3u);
    BOOST_CHECK_EQUAL(ss[0], "a");
    BOOST_CHECK(ss[1].empty());  // empty chunk between consecutive delimiters
    BOOST_CHECK_EQUAL(ss[2], "b");
}

BOOST_AUTO_TEST_CASE(load_replaces_previous_source)
{
    SplitedString ss;
    ss.load("first");
    BOOST_CHECK_EQUAL(ss.source(), "first");

    ss.load("second\nthird");
    BOOST_CHECK_EQUAL(ss.source(), "second\nthird");
    BOOST_CHECK_EQUAL(ss.size(), 2u);
    BOOST_CHECK_EQUAL(ss[0], "second");
    BOOST_CHECK_EQUAL(ss[1], "third");
}

BOOST_AUTO_TEST_CASE(load_returns_self_for_chaining)
{
    SplitedString ss;
    SplitedString& ref = ss.load("chained");
    BOOST_CHECK_EQUAL(&ref, &ss);
    BOOST_CHECK_EQUAL(ss.source(), "chained");
}

// ---- get_delimiter() — delimiter access ----

BOOST_AUTO_TEST_CASE(get_delimiter_returns_correct_delimiter)
{
    SplitedString ss;
    ss.load("a\nb\r\nc");
    // Chunk 0 delimiter is "\n"
    BOOST_CHECK_EQUAL(ss.get_delimiter(0), "\n");
    // Chunk 1 delimiter is "\r\n"
    BOOST_CHECK_EQUAL(ss.get_delimiter(1), "\r\n");
    // Chunk 2 (last) delimiter is empty
    BOOST_CHECK(ss.get_delimiter(2).empty());
}

BOOST_AUTO_TEST_CASE(get_delimiter_last_chunk_is_empty)
{
    SplitedString ss;
    ss.load("single");
    BOOST_CHECK_EQUAL(ss.size(), 1u);
    BOOST_CHECK(ss.get_delimiter(0).empty());
}

BOOST_AUTO_TEST_CASE(get_delimiter_out_of_range_throws)
{
    SplitedString ss;
    ss.load("a\nb");
    BOOST_CHECK_THROW(ss.get_delimiter(99), std::out_of_range);
}

// ---- get_chunk_index() — offset-to-chunk lookup ----

BOOST_AUTO_TEST_CASE(get_chunk_index_maps_offsets_to_chunks)
{
    SplitedString ss;
    ss.load("abc\ndef\nghi");
    // "abc" at offsets 0-2, delimiter "\n" at 3,
    // "def" at 4-6, delimiter "\n" at 7,
    // "ghi" at 8-10
    BOOST_CHECK_EQUAL(ss.get_chunk_index(0), 0u);   // start of "abc"
    BOOST_CHECK_EQUAL(ss.get_chunk_index(2), 0u);   // end of "abc"
    BOOST_CHECK_EQUAL(ss.get_chunk_index(4), 1u);   // start of "def"
    BOOST_CHECK_EQUAL(ss.get_chunk_index(8), 2u);   // start of "ghi"
}

BOOST_AUTO_TEST_CASE(get_chunk_index_offset_beyond_source_returns_npos)
{
    SplitedString ss;
    ss.load("abc");
    BOOST_CHECK_EQUAL(ss.get_chunk_index(999), std::string::npos);
}

BOOST_AUTO_TEST_CASE(get_chunk_index_empty_chunks_returns_npos)
{
    SplitedString ss;  // no source loaded → _chunks is empty
    BOOST_CHECK_EQUAL(ss.get_chunk_index(0), std::string::npos);
}

// ---- operator[] — content access ----

BOOST_AUTO_TEST_CASE(operator_bracket_returns_content_without_delimiter)
{
    SplitedString ss;
    ss.load("hello\nworld");
    // Content only, no delimiter included
    BOOST_CHECK_EQUAL(ss[0], "hello");
    BOOST_CHECK_EQUAL(ss[1], "world");
}

BOOST_AUTO_TEST_CASE(operator_bracket_out_of_range_throws)
{
    SplitedString ss;
    ss.load("a\nb");
    // Valid indices 0 and 1 should not throw.
    BOOST_CHECK_NO_THROW(ss[0]);
    BOOST_CHECK_NO_THROW(ss[1]);
    // Out-of-range index should throw.
    BOOST_CHECK_THROW(ss[99], std::out_of_range);
}

BOOST_AUTO_TEST_CASE(chunk_content_plus_delimiter_reconstructs_original)
{
    // For each chunk, content + delimiter should equal the original source
    // segment up to the start of the next chunk or end of source.
    SplitedString ss;
    ss.load("line1\nline2\r\nline3");
    std::string reconstructed;
    for (size_t i = 0; i < ss.size(); ++i) {
        reconstructed.append(ss[i]);
        reconstructed.append(ss.get_delimiter(i));
    }
    BOOST_CHECK_EQUAL(reconstructed, ss.source());
}

// ---- locate_pattern() — byte-offset pattern search ----

BOOST_AUTO_TEST_CASE(locate_pattern_finds_all_occurrences)
{
    SplitedString ss;
    ss.load("hello world\nhello again");
    auto matches = ss.locate_pattern("hello");
    BOOST_CHECK_EQUAL(matches.size(), 2u);
    // First match at byte offset 0, second at byte offset 12 (after "hello world\n")
    BOOST_CHECK_EQUAL(std::get<0>(matches[0]), 0u);
    BOOST_CHECK_EQUAL(std::get<0>(matches[1]), 12u);
}

BOOST_AUTO_TEST_CASE(locate_pattern_empty_pattern_returns_empty)
{
    SplitedString ss;
    ss.load("content");
    BOOST_CHECK(ss.locate_pattern("").empty());
}

BOOST_AUTO_TEST_CASE(locate_pattern_longer_than_source_returns_empty)
{
    SplitedString ss;
    ss.load("ab");
    BOOST_CHECK(ss.locate_pattern("abcdefgh").empty());
}

BOOST_AUTO_TEST_CASE(locate_pattern_no_match_returns_empty)
{
    SplitedString ss;
    ss.load("abcdef");
    BOOST_CHECK(ss.locate_pattern("xyz").empty());
}

// ---- locate_pattern_chunk() — chunk-index pattern search ----

BOOST_AUTO_TEST_CASE(locate_pattern_chunk_maps_to_chunks)
{
    SplitedString ss;
    ss.load("hello\nworld\nhello");
    auto matches = ss.locate_pattern_chunk("hello");
    BOOST_CHECK_EQUAL(matches.size(), 2u);
    // "hello" on chunk 0 and chunk 2
    BOOST_CHECK_EQUAL(std::get<0>(matches[0]), 0u);
    BOOST_CHECK_EQUAL(std::get<0>(matches[1]), 2u);
}

BOOST_AUTO_TEST_CASE(locate_pattern_chunk_cross_line_match)
{
    // A pattern that spans a line break maps to both chunks.
    SplitedString ss;
    ss.load("abc\ndef");
    auto matches = ss.locate_pattern_chunk("c\nd");
    BOOST_REQUIRE_EQUAL(matches.size(), 1u);
    BOOST_CHECK_EQUAL(std::get<0>(matches[0]), 0u);  // starts in chunk 0
    BOOST_CHECK_EQUAL(std::get<1>(matches[0]), 1u);  // ends in chunk 1
}

// ---- size() and source() consistency ----

BOOST_AUTO_TEST_CASE(size_and_source_consistent)
{
    SplitedString ss;
    ss.load("one\ntwo\nthree");
    BOOST_CHECK_EQUAL(ss.size(), 3u);
    BOOST_CHECK_EQUAL(ss.source(), "one\ntwo\nthree");
}

BOOST_AUTO_TEST_CASE(source_unchanged_after_construction)
{
    // source() returns the exact string passed to load().
    std::string original = "exact\ncontent\r\nhere";
    SplitedString ss;
    ss.load(original);
    BOOST_CHECK_EQUAL(ss.source(), original);
}

// ---- Iterator interface ----

BOOST_AUTO_TEST_CASE(iterator_range_for)
{
    SplitedString ss;
    ss.load("a\nb\nc");
    std::vector<std::string> result;
    for (auto line : ss) {
        result.emplace_back(line);
    }
    BOOST_REQUIRE_EQUAL(result.size(), 3u);
    BOOST_CHECK_EQUAL(result[0], "a");
    BOOST_CHECK_EQUAL(result[1], "b");
    BOOST_CHECK_EQUAL(result[2], "c");
}

BOOST_AUTO_TEST_CASE(iterator_random_access)
{
    SplitedString ss;
    ss.load("zero\none\ntwo\nthree\nfour");
    auto it = ss.begin();
    BOOST_CHECK_EQUAL(*it, "zero");
    it += 2;
    BOOST_CHECK_EQUAL(*it, "two");
    it -= 1;
    BOOST_CHECK_EQUAL(*it, "one");
    BOOST_CHECK_EQUAL(it[2], "three");  // offset from current position
}

BOOST_AUTO_TEST_CASE(iterator_comparison)
{
    SplitedString ss;
    ss.load("a\nb\nc");
    auto it1 = ss.begin();
    auto it2 = ss.begin();
    BOOST_CHECK(it1 == it2);
    ++it2;
    BOOST_CHECK(it1 != it2);
    BOOST_CHECK(it1 < it2);
    BOOST_CHECK(it2 > it1);
    BOOST_CHECK(it1 <= it2);
    BOOST_CHECK(it2 >= it1);
}

BOOST_AUTO_TEST_CASE(iterator_distance)
{
    SplitedString ss;
    ss.load("a\nb\nc\nd\ne");
    BOOST_CHECK_EQUAL(ss.end() - ss.begin(), static_cast<std::ptrdiff_t>(ss.size()));
}

BOOST_AUTO_TEST_CASE(reverse_iteration)
{
    SplitedString ss;
    ss.load("first\nsecond\nthird");
    std::vector<std::string> result;
    for (auto it = ss.rbegin(); it != ss.rend(); ++it) {
        result.emplace_back(*it);
    }
    BOOST_REQUIRE_EQUAL(result.size(), 3u);
    BOOST_CHECK_EQUAL(result[0], "third");
    BOOST_CHECK_EQUAL(result[1], "second");
    BOOST_CHECK_EQUAL(result[2], "first");
}

BOOST_AUTO_TEST_CASE(cbegin_cend_same_as_begin_end)
{
    SplitedString ss;
    ss.load("a\nb\nc");
    // const_iterator is the same type as iterator for SplitedString
    std::vector<std::string> result_c;
    for (auto it = ss.cbegin(); it != ss.cend(); ++it) {
        result_c.emplace_back(*it);
    }
    std::vector<std::string> result;
    for (auto it = ss.begin(); it != ss.end(); ++it) {
        result.emplace_back(*it);
    }
    BOOST_CHECK_EQUAL_COLLECTIONS(result_c.begin(), result_c.end(),
                                  result.begin(), result.end());
}

BOOST_AUTO_TEST_CASE(iterator_empty_source)
{
    SplitedString ss;
    ss.load("");
    // One empty chunk — begin() != end() since size() == 1.
    size_t count = 0;
    for (auto line : ss) {
        BOOST_CHECK(line.empty());
        ++count;
    }
    BOOST_CHECK_EQUAL(count, 1u);
}

// ---- read() — file loading ----

BOOST_AUTO_TEST_CASE(read_from_file)
{
    // Create a temporary file with known content.
    auto tmp_path = std::filesystem::temp_directory_path() / "cpptools_test_split.txt";
    {
        std::ofstream ofs(tmp_path);
        ofs << "line1\nline2\nline3";
    }

    SplitedString ss;
    ss.read(tmp_path);
    BOOST_CHECK_EQUAL(ss.size(), 3u);
    BOOST_CHECK_EQUAL(ss[0], "line1");
    BOOST_CHECK_EQUAL(ss[1], "line2");
    BOOST_CHECK_EQUAL(ss[2], "line3");

    std::error_code ec;
    std::filesystem::remove(tmp_path, ec);
}

BOOST_AUTO_TEST_CASE(read_nonexistent_file_throws)
{
    SplitedString ss;
    BOOST_CHECK_THROW(
        ss.read(std::filesystem::path("/nonexistent/path/for/split/test.txt")),
        std::runtime_error
    );
}

BOOST_AUTO_TEST_CASE(read_empty_file)
{
    auto tmp_path = std::filesystem::temp_directory_path() / "cpptools_test_empty.txt";
    {
        std::ofstream ofs(tmp_path);
        // Write nothing
    }

    SplitedString ss;
    ss.read(tmp_path);
    BOOST_CHECK_EQUAL(ss.size(), 1u);
    BOOST_CHECK(ss[0].empty());

    std::error_code ec;
    std::filesystem::remove(tmp_path, ec);
}

// ---- Complex source patterns ----

BOOST_AUTO_TEST_CASE(only_delimiters_source)
{
    // Source consisting only of delimiters.
    SplitedString ss;
    ss.load("\n\n\n");
    BOOST_CHECK_EQUAL(ss.size(), 4u);  // 3 empty chunks + 1 trailing empty
    for (size_t i = 0; i < ss.size(); ++i) {
        BOOST_CHECK(ss[i].empty());
    }
    // First 3 chunks have "\n" delimiter, last has empty delimiter
    BOOST_CHECK_EQUAL(ss.get_delimiter(0), "\n");
    BOOST_CHECK_EQUAL(ss.get_delimiter(1), "\n");
    BOOST_CHECK_EQUAL(ss.get_delimiter(2), "\n");
    BOOST_CHECK(ss.get_delimiter(3).empty());
}

BOOST_AUTO_TEST_CASE(source_with_empty_lines)
{
    SplitedString ss;
    ss.load("a\n\nb\n\nc\n");
    BOOST_CHECK_EQUAL(ss.size(), 6u);
    BOOST_CHECK_EQUAL(ss[0], "a");
    BOOST_CHECK(ss[1].empty());
    BOOST_CHECK_EQUAL(ss[2], "b");
    BOOST_CHECK(ss[3].empty());
    BOOST_CHECK_EQUAL(ss[4], "c");
    BOOST_CHECK(ss[5].empty());  // trailing empty after final "\n"
}

BOOST_AUTO_TEST_CASE(crlf_takes_precedence_over_lf)
{
    // When "\r\n" and "\n" both match, the longer match ("\r\n") should win
    // to prevent "\r" from appearing in the content.
    SplitedString ss;
    ss.load("a\r\nb");
    BOOST_CHECK_EQUAL(ss.size(), 2u);
    BOOST_CHECK_EQUAL(ss[0], "a");
    BOOST_CHECK_EQUAL(ss[1], "b");
    // The delimiter after "a" should be "\r\n", not "\n"
    BOOST_CHECK_EQUAL(ss.get_delimiter(0), "\r\n");
}

BOOST_AUTO_TEST_CASE(single_line_no_delimiter)
{
    SplitedString ss;
    ss.load("just one line");
    BOOST_CHECK_EQUAL(ss.size(), 1u);
    BOOST_CHECK_EQUAL(ss[0], "just one line");
    BOOST_CHECK(ss.get_delimiter(0).empty());
}

// ---- Multi-delimiter splitting ----
//
// These tests exercise the core multi-delimiter feature: constructing a
// SplitedString with multiple delimiter patterns and verifying correct
// chunk decomposition, delimiter identification, and edge cases.
//
// The non_overlapping(intervals, prefer_longer=true) call inside
// _build_chunks() ensures that when two delimiters start at the same
// byte offset, the longer one wins. This is critical for correctness:
// without it, "\r\n" would be matched first by "\n" (producing a stray
// "\r" in the content), then "\r\n" would be skipped as overlapping.

// --- Nested / prefix delimiters ---

BOOST_AUTO_TEST_CASE(multi_delim_nested_longer_wins)
{
    // "\n\n" (paragraph break) vs "\n" (line break).
    // Where "\n\n" appears it should be one delimiter, not two "\n"s.
    SplitedString ss({"\n\n", "\n"});
    ss.load("a\n\nb");
    BOOST_CHECK_EQUAL(ss.size(), 2u);
    BOOST_CHECK_EQUAL(ss[0], "a");
    BOOST_CHECK_EQUAL(ss[1], "b");
    BOOST_CHECK_EQUAL(ss.get_delimiter(0), "\n\n");
}

BOOST_AUTO_TEST_CASE(multi_delim_nested_paragraphs_and_lines)
{
    // Mixed: paragraph breaks and single line breaks in one source.
    SplitedString ss({"\n\n", "\n"});
    ss.load("line1\nline2\n\nparagraph2");
    BOOST_CHECK_EQUAL(ss.size(), 3u);
    BOOST_CHECK_EQUAL(ss[0], "line1");
    BOOST_CHECK_EQUAL(ss[1], "line2");
    BOOST_CHECK_EQUAL(ss[2], "paragraph2");
    // Chunk 0 delimiter is "\n" (single line break)
    BOOST_CHECK_EQUAL(ss.get_delimiter(0), "\n");
    // Chunk 1 delimiter is "\n\n" (paragraph break, longer wins over single "\n")
    BOOST_CHECK_EQUAL(ss.get_delimiter(1), "\n\n");
}

BOOST_AUTO_TEST_CASE(multi_delim_nested_three_levels)
{
    // Three delimiters: "===" (section), "==" (subsection), "=" (separator).
    // The longest matching pattern should always win.
    SplitedString ss({"===", "==", "="});
    ss.load("a=b==c===d");
    BOOST_CHECK_EQUAL(ss.size(), 4u);
    BOOST_CHECK_EQUAL(ss[0], "a");
    BOOST_CHECK_EQUAL(ss[1], "b");
    BOOST_CHECK_EQUAL(ss[2], "c");
    BOOST_CHECK_EQUAL(ss[3], "d");
    BOOST_CHECK_EQUAL(ss.get_delimiter(0), "=");
    BOOST_CHECK_EQUAL(ss.get_delimiter(1), "==");
    BOOST_CHECK_EQUAL(ss.get_delimiter(2), "===");
    BOOST_CHECK(ss.get_delimiter(3).empty());
}

BOOST_AUTO_TEST_CASE(multi_delim_nested_double_char)
{
    // Delimiters where one is a prefix of another, two-char variants.
    SplitedString ss({"==", "="});
    ss.load("a==b=c");
    BOOST_CHECK_EQUAL(ss.size(), 3u);
    BOOST_CHECK_EQUAL(ss[0], "a");
    BOOST_CHECK_EQUAL(ss[1], "b");
    BOOST_CHECK_EQUAL(ss[2], "c");
    // "==" at offset 1-2 is longer than "=" at 1-1, so "==" wins
    BOOST_CHECK_EQUAL(ss.get_delimiter(0), "==");
    BOOST_CHECK_EQUAL(ss.get_delimiter(1), "=");
}

// --- Single-char multi-delimiters ---

BOOST_AUTO_TEST_CASE(multi_delim_single_char_separators)
{
    // Multiple single-character delimiters: comma, semicolon, pipe.
    SplitedString ss({",", ";", "|"});
    ss.load("a,b;c|d");
    BOOST_CHECK_EQUAL(ss.size(), 4u);
    BOOST_CHECK_EQUAL(ss[0], "a");
    BOOST_CHECK_EQUAL(ss[1], "b");
    BOOST_CHECK_EQUAL(ss[2], "c");
    BOOST_CHECK_EQUAL(ss[3], "d");
    BOOST_CHECK_EQUAL(ss.get_delimiter(0), ",");
    BOOST_CHECK_EQUAL(ss.get_delimiter(1), ";");
    BOOST_CHECK_EQUAL(ss.get_delimiter(2), "|");
    BOOST_CHECK(ss.get_delimiter(3).empty());
}

BOOST_AUTO_TEST_CASE(multi_delim_single_char_adjacent_different)
{
    // Adjacent delimiters of different types — each produces an empty chunk.
    SplitedString ss({",", ";"});
    ss.load("a,;b");
    BOOST_CHECK_EQUAL(ss.size(), 3u);
    BOOST_CHECK_EQUAL(ss[0], "a");
    BOOST_CHECK(ss[1].empty());  // between "," and ";"
    BOOST_CHECK_EQUAL(ss[2], "b");
    BOOST_CHECK_EQUAL(ss.get_delimiter(0), ",");
    BOOST_CHECK_EQUAL(ss.get_delimiter(1), ";");
}

// --- All-delimiter source ---

BOOST_AUTO_TEST_CASE(multi_delim_source_is_all_delimiters)
{
    // Source consists entirely of delimiters — all chunks should be empty.
    SplitedString ss({"::", ";;"});
    ss.load("::::;;");
    BOOST_CHECK_EQUAL(ss.size(), 4u);  // "::" at 0-1, "::" at 2u-3u, ";;" at 4u-5u, trailing empty at 6u
    for (size_t i = 0; i < ss.size(); ++i) {
        BOOST_CHECK(ss[i].empty());
    }
    BOOST_CHECK_EQUAL(ss.get_delimiter(0), "::");
    BOOST_CHECK_EQUAL(ss.get_delimiter(1), "::");
    BOOST_CHECK_EQUAL(ss.get_delimiter(2), ";;");
    BOOST_CHECK(ss.get_delimiter(3).empty());
}

BOOST_AUTO_TEST_CASE(multi_delim_only_delimiters_mixed_lengths)
{
    // Source is only delimiters, with mixed single and multi-char.
    SplitedString ss({"\n\n", "\n"});
    ss.load("\n\n\n");
    // Content: "\n\n\n"
    // Matches: "\n\n" at (0,1), "\n" at (0,0) [overlapped, shorter loses],
    //          "\n" at (2,2) [kept]
    BOOST_CHECK_EQUAL(ss.size(), 3u);
    BOOST_CHECK(ss[0].empty());
    BOOST_CHECK(ss[1].empty());
    BOOST_CHECK(ss[2].empty());
    BOOST_CHECK_EQUAL(ss.get_delimiter(0), "\n\n");
    BOOST_CHECK_EQUAL(ss.get_delimiter(1), "\n");
}

// --- Empty string in delimiter set ---

BOOST_AUTO_TEST_CASE(multi_delim_empty_string_ignored)
{
    // Empty patterns are silently ignored by add_pattern().
    // Constructing with {"", "\n"} should behave identically to {"\n"}.
    SplitedString ss({"", "\n"});
    ss.load("a\nb");
    BOOST_CHECK_EQUAL(ss.size(), 2u);
    BOOST_CHECK_EQUAL(ss[0], "a");
    BOOST_CHECK_EQUAL(ss[1], "b");
    BOOST_CHECK_EQUAL(ss.get_delimiter(0), "\n");
}

// --- Shared prefix delimiters (same starting character) ---

BOOST_AUTO_TEST_CASE(multi_delim_shared_prefix)
{
    // Delimiters "//" and "/*" both start with '/'.
    SplitedString ss({"//", "/*"});
    ss.load("code//comment\ncode/*block*/end");
    // Two delimiters → three chunks.
    BOOST_CHECK_EQUAL(ss.size(), 3u);
    BOOST_CHECK_EQUAL(ss[0], "code");
    BOOST_CHECK_EQUAL(ss[1], "comment\ncode");
    BOOST_CHECK_EQUAL(ss[2], "block*/end");
    // Delimiters
    BOOST_CHECK_EQUAL(ss.get_delimiter(0), "//");
    BOOST_CHECK_EQUAL(ss.get_delimiter(1), "/*");
    BOOST_CHECK(ss.get_delimiter(2).empty());
}

BOOST_AUTO_TEST_CASE(multi_delim_shared_prefix_adjacent)
{
    // Adjacent shared-prefix delimiters.
    SplitedString ss({"//", "/*"});
    ss.load("a///*b");
    BOOST_CHECK_EQUAL(ss.size(), 3u);
    BOOST_CHECK_EQUAL(ss[0], "a");
    BOOST_CHECK(ss[1].empty());  // between "//" and "/*"
    BOOST_CHECK_EQUAL(ss[2], "b");
    BOOST_CHECK_EQUAL(ss.get_delimiter(0), "//");
    BOOST_CHECK_EQUAL(ss.get_delimiter(1), "/*");
}

// --- Delimiter at source boundaries ---

BOOST_AUTO_TEST_CASE(multi_delim_at_start)
{
    // Source starts with a delimiter — first chunk is empty.
    SplitedString ss({"::"});
    ss.load("::content");
    BOOST_CHECK_EQUAL(ss.size(), 2u);
    BOOST_CHECK(ss[0].empty());
    BOOST_CHECK_EQUAL(ss[1], "content");
    BOOST_CHECK_EQUAL(ss.get_delimiter(0), "::");
}

BOOST_AUTO_TEST_CASE(multi_delim_at_start_and_end)
{
    // Delimiters at both boundaries.
    SplitedString ss({"##"});
    ss.load("##middle##");
    BOOST_CHECK_EQUAL(ss.size(), 3u);
    BOOST_CHECK(ss[0].empty());     // before leading "##"
    BOOST_CHECK_EQUAL(ss[1], "middle");
    BOOST_CHECK(ss[2].empty());     // after trailing "##"
    BOOST_CHECK_EQUAL(ss.get_delimiter(0), "##");
    BOOST_CHECK_EQUAL(ss.get_delimiter(1), "##");
    BOOST_CHECK(ss.get_delimiter(2).empty());
}

BOOST_AUTO_TEST_CASE(multi_delim_end_only)
{
    // Delimiter only at the end — trailing empty chunk.
    SplitedString ss({"<END>"});
    ss.load("body<END>");
    BOOST_CHECK_EQUAL(ss.size(), 2u);
    BOOST_CHECK_EQUAL(ss[0], "body");
    BOOST_CHECK(ss[1].empty());
    BOOST_CHECK_EQUAL(ss.get_delimiter(0), "<END>");
    BOOST_CHECK(ss.get_delimiter(1).empty());
}

// --- Reconstruct with multi-char delimiters ---

BOOST_AUTO_TEST_CASE(multi_delim_reconstruct_mixed_delimiters)
{
    // chunk content + chunk delimiter should perfectly reconstruct source.
    SplitedString ss({"::", ";;", "//"});
    std::string original = "a::b;;c//d";
    ss.load(original);
    std::string reconstructed;
    for (size_t i = 0; i < ss.size(); ++i) {
        reconstructed.append(ss[i]);
        reconstructed.append(ss.get_delimiter(i));
    }
    BOOST_CHECK_EQUAL(reconstructed, original);
}

// --- Cross-delimiter locate_pattern ---

BOOST_AUTO_TEST_CASE(multi_delim_locate_pattern_chunk_across_different_delimiters)
{
    // locate_pattern_chunk should work across any delimiter type.
    SplitedString ss({"=", "==", "==="});
    ss.load("find=a==b===find");
    auto matches = ss.locate_pattern_chunk("find");
    BOOST_CHECK_EQUAL(matches.size(), 2u);
    BOOST_CHECK_EQUAL(std::get<0>(matches[0]), 0u);
    BOOST_CHECK_EQUAL(std::get<0>(matches[1]), 3u);
}

// --- locate_pattern(use_regex) / locate_pattern_chunk(use_regex) ---

BOOST_AUTO_TEST_CASE(locate_pattern_regex_finds_all_occurrences)
{
    SplitedString ss;
    ss.load("foo123\nbar456\nfoo789");
    // "foo" followed by digits, only on the foo lines.
    auto matches = ss.locate_pattern("foo[0-9]+", /*use_regex=*/true);
    BOOST_REQUIRE_EQUAL(matches.size(), 2u);
    // byte offsets: "foo123" at 0..5; "foo789" at 14..19 (after "foo123\nbar456\n")
    BOOST_CHECK_EQUAL(std::get<0>(matches[0]), 0u);
    BOOST_CHECK_EQUAL(std::get<1>(matches[0]), 5u);
    BOOST_CHECK_EQUAL(std::get<0>(matches[1]), 14u);
    BOOST_CHECK_EQUAL(std::get<1>(matches[1]), 19u);
}

BOOST_AUTO_TEST_CASE(locate_pattern_regex_is_non_overlapping)
{
    // "aa" on "aaaa" — non-overlapping gives two matches (positions 0 and 2),
    // not three (0, 1, 2). Mirrors the plain-substring semantics.
    SplitedString ss;
    ss.load("aaaa");
    auto matches = ss.locate_pattern("aa", /*use_regex=*/true);
    BOOST_REQUIRE_EQUAL(matches.size(), 2u);
    BOOST_CHECK_EQUAL(std::get<0>(matches[0]), 0u);
    BOOST_CHECK_EQUAL(std::get<0>(matches[1]), 2u);
}

BOOST_AUTO_TEST_CASE(locate_pattern_regex_skips_zero_length_matches)
{
    // "a*" matches empty everywhere; only the non-empty run "aaa" should be
    // reported, as a single match.
    SplitedString ss;
    ss.load("baaab");
    auto matches = ss.locate_pattern("a+", /*use_regex=*/true);
    BOOST_REQUIRE_EQUAL(matches.size(), 1u);
    BOOST_CHECK_EQUAL(std::get<0>(matches[0]), 1u);
    BOOST_CHECK_EQUAL(std::get<1>(matches[0]), 3u);
}

BOOST_AUTO_TEST_CASE(locate_pattern_regex_invalid_returns_empty)
{
    // An unbalanced group is an invalid regex; result must be empty (and the
    // noexcept call must not terminate).
    SplitedString ss;
    ss.load("content");
    auto matches = ss.locate_pattern("(unclosed", /*use_regex=*/true);
    BOOST_CHECK(matches.empty());
}

BOOST_AUTO_TEST_CASE(locate_pattern_regex_no_match_returns_empty)
{
    SplitedString ss;
    ss.load("abcdef");
    BOOST_CHECK(ss.locate_pattern("[0-9]+", /*use_regex=*/true).empty());
}

BOOST_AUTO_TEST_CASE(locate_pattern_regex_cross_line_match)
{
    // A regex that matches across a line break maps to both chunks. Note:
    // ECMAScript "." does not match '\n', so use [\s\S] to span the break.
    SplitedString ss;
    ss.load("abc\ndef");
    auto matches = ss.locate_pattern_chunk("c[\\s\\S]d", /*use_regex=*/true);
    BOOST_REQUIRE_EQUAL(matches.size(), 1u);
    BOOST_CHECK_EQUAL(std::get<0>(matches[0]), 0u);  // starts in chunk 0
    BOOST_CHECK_EQUAL(std::get<1>(matches[0]), 1u);  // ends in chunk 1
}

BOOST_AUTO_TEST_CASE(locate_pattern_chunk_regex_maps_to_chunks)
{
    SplitedString ss;
    ss.load("int x = 1;\nint y = 2;\nfloat z = 3;");
    // Match "int" declarations only.
    auto matches = ss.locate_pattern_chunk("int\\s+\\w+", /*use_regex=*/true);
    BOOST_REQUIRE_EQUAL(matches.size(), 2u);
    BOOST_CHECK_EQUAL(std::get<0>(matches[0]), 0u);
    BOOST_CHECK_EQUAL(std::get<0>(matches[1]), 1u);
}

BOOST_AUTO_TEST_CASE(locate_pattern_regex_default_is_plain_substring)
{
    // Without use_regex, a regex metacharacter is treated literally.
    SplitedString ss;
    ss.load("a.b.c");
    auto plain = ss.locate_pattern(".", /*use_regex=*/false);
    auto regex = ss.locate_pattern(".", /*use_regex=*/true);
    // Plain: one literal "." match at position 1 (first '.'; non-overlapping,
    // there are two '.' so size 2). Regex: "." matches every single char.
    BOOST_CHECK_EQUAL(plain.size(), 2u);
    BOOST_CHECK_EQUAL(std::get<0>(plain[0]), 1u);
    BOOST_REQUIRE_EQUAL(regex.size(), 5u);
}

BOOST_AUTO_TEST_SUITE_END()
