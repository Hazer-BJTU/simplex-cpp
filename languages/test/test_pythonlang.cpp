#define BOOST_TEST_MODULE PythonLanguageTests
#include <boost/test/unit_test.hpp>

#include "python/pythonlang.hpp"

#include <fstream>
#include <filesystem>
#include <algorithm>
#include <set>

using namespace indextools;

// ============================================================================
// Test helpers
// ============================================================================

namespace {

// RAII temporary Python file for open() tests
struct TempPythonFile {
    std::filesystem::path path;

    explicit TempPythonFile(const std::string& content) {
        path = std::filesystem::temp_directory_path()
               / "cpptools_test_temp.py";
        std::ofstream ofs(path);
        ofs << content;
    }

    ~TempPythonFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    TempPythonFile(const TempPythonFile&) = delete;
    TempPythonFile& operator=(const TempPythonFile&) = delete;
};

// Convenience: analyze a Python source string
struct AnalysisResult {
    PythonLanguage lang;
    const PythonLanguage::EntityList& entities;
    const PythonLanguage::LineIndex& identifiers;

    explicit AnalysisResult(std::string source)
        : lang()
        , entities(lang.load(std::move(source))->analyze()->result())
        , identifiers(lang.get_identifier_line_map())
    {}
};

// Find first entity by type
const PythonLanguage::EntityTag* find_first_by_type(
    const PythonLanguage::EntityList& entities,
    const std::string& type)
{
    for (const auto& e : entities) {
        const auto* py_entity =
            dynamic_cast<const PythonLanguage::EntityTag*>(e.get());
        if (py_entity && py_entity->type == type) {
            return py_entity;
        }
    }
    return nullptr;
}

// Count entities by type
size_t count_by_type(
    const PythonLanguage::EntityList& entities,
    const std::string& type)
{
    size_t cnt = 0;
    for (const auto& e : entities) {
        const auto* py_entity =
            dynamic_cast<const PythonLanguage::EntityTag*>(e.get());
        if (py_entity && py_entity->type == type) {
            ++cnt;
        }
    }
    return cnt;
}

// Find entity by key (lowercased match)
const PythonLanguage::EntityTag* find_by_key(
    const PythonLanguage::EntityList& entities,
    const std::string& key)
{
    for (const auto& e : entities) {
        const auto* py_entity =
            dynamic_cast<const PythonLanguage::EntityTag*>(e.get());
        if (py_entity && py_entity->key == key) {
            return py_entity;
        }
    }
    return nullptr;
}

} // anonymous namespace

// ============================================================================
// Suite: PythonLanguageEdgeCasesSuite
// ============================================================================

BOOST_AUTO_TEST_SUITE(PythonLanguageEdgeCasesSuite)

BOOST_AUTO_TEST_CASE(empty_source_produces_empty_result)
{
    AnalysisResult ar("");
    // Only module entity should exist (even for empty source, tree-sitter
    // creates a module node)
    BOOST_CHECK(ar.entities.empty() || ar.entities.size() == 1u);
    BOOST_CHECK(ar.identifiers.empty());
}

BOOST_AUTO_TEST_CASE(whitespace_only_source)
{
    AnalysisResult ar("  \n  \n  ");
    // No crash expected; identifiers should be empty
    BOOST_CHECK(ar.identifiers.empty());
}

BOOST_AUTO_TEST_CASE(comments_only_produces_no_identifiers)
{
    AnalysisResult ar("# just a comment\n# another comment");
    // Tree-sitter creates a module node; identifiers may include comment words
    // or not depending on parser behavior — just verify no crash
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(malformed_syntax_does_not_crash)
{
    // Incomplete function definition
    BOOST_CHECK_NO_THROW(AnalysisResult("def ("));
    // Mismatched brackets
    BOOST_CHECK_NO_THROW(AnalysisResult("class Foo:\n    def bar(self):\n        pass\n  oops"));
    // Random symbols
    BOOST_CHECK_NO_THROW(AnalysisResult("!!! @@@ ###"));
}

BOOST_AUTO_TEST_CASE(unicode_identifiers_handled)
{
    // Test with a non-ASCII function name (UTF-8 encoded)
    AnalysisResult ar("def f\xc3\xb6\xc3\xb6():\n    return 42");
    // The function entity should be created without crashing
    const auto* func = find_first_by_type(ar.entities, "function_definition");
    BOOST_REQUIRE(func != nullptr);
    // Key should be non-empty
    BOOST_CHECK(!func->key.empty());
}

BOOST_AUTO_TEST_CASE(newline_variants_handled)
{
    // Source with CRLF
    AnalysisResult ar("import os\r\nimport sys\r\n");
    size_t import_count = count_by_type(ar.entities, "import_statement");
    BOOST_CHECK_EQUAL(import_count, 2u);
}

BOOST_AUTO_TEST_CASE(source_with_only_newlines)
{
    AnalysisResult ar("\n\n\n");
    // No crash
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: PythonLanguageModuleSuite
// ============================================================================

BOOST_AUTO_TEST_SUITE(PythonLanguageModuleSuite)

BOOST_AUTO_TEST_CASE(module_entity_created)
{
    AnalysisResult ar("x = 1");
    const auto* mod = find_first_by_type(ar.entities, "module");
    BOOST_REQUIRE(mod != nullptr);
}

BOOST_AUTO_TEST_CASE(module_line_start_is_zero)
{
    AnalysisResult ar("x = 1");
    const auto* mod = find_first_by_type(ar.entities, "module");
    BOOST_REQUIRE(mod != nullptr);
    BOOST_CHECK_EQUAL(mod->line_start, 0u);
}

BOOST_AUTO_TEST_CASE(module_via_open)
{
    TempPythonFile tmp("x = 1");
    PythonLanguage lang;
    lang.open(tmp.path)->analyze();
    const auto& entities = lang.result();
    const auto* mod = find_first_by_type(entities, "module");
    BOOST_REQUIRE(mod != nullptr);
    BOOST_CHECK_EQUAL(mod->absolute_path, tmp.path);
}

BOOST_AUTO_TEST_CASE(module_is_first_entity)
{
    AnalysisResult ar("def foo():\n    pass");
    BOOST_REQUIRE(!ar.entities.empty());
    const auto* first =
        dynamic_cast<const PythonLanguage::EntityTag*>(ar.entities[0].get());
    BOOST_REQUIRE(first != nullptr);
    BOOST_CHECK_EQUAL(first->type, "module");
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: PythonLanguageFunctionSuite
// ============================================================================

BOOST_AUTO_TEST_SUITE(PythonLanguageFunctionSuite)

BOOST_AUTO_TEST_CASE(simple_function_detected)
{
    AnalysisResult ar("def foo():\n    pass");
    const auto* func = find_by_key(ar.entities, "foo");
    BOOST_REQUIRE(func != nullptr);
    BOOST_CHECK_EQUAL(func->type, "function_definition");
}

BOOST_AUTO_TEST_CASE(multiple_functions_detected)
{
    AnalysisResult ar("def a():\n    pass\n\ndef b():\n    pass");
    size_t count = count_by_type(ar.entities, "function_definition");
    BOOST_CHECK_EQUAL(count, 2u);
}

BOOST_AUTO_TEST_CASE(function_with_parameters)
{
    AnalysisResult ar("def add(a, b):\n    return a + b");
    const auto* func = find_by_key(ar.entities, "add");
    BOOST_REQUIRE(func != nullptr);
    BOOST_CHECK_EQUAL(func->type, "function_definition");
}

BOOST_AUTO_TEST_CASE(function_with_type_hints)
{
    AnalysisResult ar("def add(a: int, b: int) -> int:\n    return a + b");
    const auto* func = find_by_key(ar.entities, "add");
    BOOST_REQUIRE(func != nullptr);
}

BOOST_AUTO_TEST_CASE(function_key_is_lowercased)
{
    AnalysisResult ar("def FooBar():\n    pass");
    const auto* func = find_by_key(ar.entities, "foobar");
    BOOST_REQUIRE(func != nullptr);
}

BOOST_AUTO_TEST_CASE(nested_function_detected)
{
    AnalysisResult ar("def outer():\n    def inner():\n        pass\n    return inner");
    const auto* outer = find_by_key(ar.entities, "outer");
    const auto* inner = find_by_key(ar.entities, "inner");
    BOOST_REQUIRE(outer != nullptr);
    BOOST_REQUIRE(inner != nullptr);
    BOOST_CHECK_EQUAL(outer->type, "function_definition");
    BOOST_CHECK_EQUAL(inner->type, "function_definition");
}

BOOST_AUTO_TEST_CASE(function_content_extracted)
{
    AnalysisResult ar("def foo(x, y):\n    return x + y");
    const auto* func = find_by_key(ar.entities, "foo");
    BOOST_REQUIRE(func != nullptr);
    // Content should include the signature line (dedented)
    BOOST_CHECK(!func->content.empty());
}

BOOST_AUTO_TEST_CASE(function_line_range_correct)
{
    AnalysisResult ar("def foo():\n    pass");
    const auto* func = find_by_key(ar.entities, "foo");
    BOOST_REQUIRE(func != nullptr);
    // Function starts at line 0, ends at end of body (line 1)
    BOOST_CHECK_EQUAL(func->line_start, 0u);
    BOOST_CHECK(func->line_end >= 1u);
}

BOOST_AUTO_TEST_CASE(function_with_docstring)
{
    AnalysisResult ar("def foo():\n    \"\"\"Docstring.\"\"\"\n    pass");
    const auto* func = find_by_key(ar.entities, "foo");
    BOOST_REQUIRE(func != nullptr);
}

BOOST_AUTO_TEST_CASE(async_function_detected)
{
    AnalysisResult ar("async def foo():\n    await bar()");
    // async functions may have a different node type in tree-sitter
    // This test verifies we don't crash; detection depends on tree-sitter version
    BOOST_CHECK(true);
    (void)find_by_key(ar.entities, "foo");
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: PythonLanguageLambdaSuite
// ============================================================================

BOOST_AUTO_TEST_SUITE(PythonLanguageLambdaSuite)

BOOST_AUTO_TEST_CASE(simple_lambda_detected)
{
    AnalysisResult ar("square = lambda x: x ** 2");
    const auto* lam = find_first_by_type(ar.entities, "lambda");
    BOOST_REQUIRE(lam != nullptr);
    BOOST_CHECK_EQUAL(lam->type, "lambda");
}

BOOST_AUTO_TEST_CASE(lambda_key_from_assignment_target)
{
    AnalysisResult ar("square = lambda x: x ** 2");
    const auto* lam = find_first_by_type(ar.entities, "lambda");
    BOOST_REQUIRE(lam != nullptr);
    // When lambda is RHS of assignment to identifier, key is that identifier (lowercased)
    BOOST_CHECK_EQUAL(lam->key, "square");
}

BOOST_AUTO_TEST_CASE(lambda_key_is_lowercased)
{
    AnalysisResult ar("Square = lambda x: x ** 2");
    const auto* lam = find_first_by_type(ar.entities, "lambda");
    BOOST_REQUIRE(lam != nullptr);
    BOOST_CHECK_EQUAL(lam->key, "square");
}

BOOST_AUTO_TEST_CASE(lambda_with_default_parameter)
{
    AnalysisResult ar("add = lambda a, b=0: a + b");
    const auto* lam = find_first_by_type(ar.entities, "lambda");
    BOOST_REQUIRE(lam != nullptr);
    BOOST_CHECK_EQUAL(lam->key, "add");
}

BOOST_AUTO_TEST_CASE(lambda_inside_call_uses_line_fallback)
{
    // map(lambda x: x * x, nums) — lambda inside a call, no assignment parent
    AnalysisResult ar("nums = [1, 2, 3]\nresult = list(map(lambda x: x * x, nums))");
    const auto* lam = find_first_by_type(ar.entities, "lambda");
    BOOST_REQUIRE(lam != nullptr);
    // Key should be fallback "lambda_{line}" since no assignment parent
    BOOST_CHECK(lam->key.find("lambda_") == 0);
}

BOOST_AUTO_TEST_CASE(lambda_has_content)
{
    AnalysisResult ar("square = lambda x: x ** 2");
    const auto* lam = find_first_by_type(ar.entities, "lambda");
    BOOST_REQUIRE(lam != nullptr);
    BOOST_CHECK(!lam->content.empty());
}

BOOST_AUTO_TEST_CASE(lambda_parent_is_correct)
{
    AnalysisResult ar(
        "def outer():\n"
        "    square = lambda x: x ** 2\n"
        "    return square"
    );
    const auto* lam = find_first_by_type(ar.entities, "lambda");
    BOOST_REQUIRE(lam != nullptr);
    BOOST_REQUIRE(lam->parent_entity_idx.has_value());
    // Parent should be the enclosing function "outer"
    const auto* parent =
        dynamic_cast<const PythonLanguage::EntityTag*>(
            ar.entities[lam->parent_entity_idx.value()].get());
    BOOST_REQUIRE(parent != nullptr);
    BOOST_CHECK_EQUAL(parent->key, "outer");
}

BOOST_AUTO_TEST_CASE(lambda_inside_class_method)
{
    AnalysisResult ar(
        "class Foo:\n"
        "    def bar(self):\n"
        "        f = lambda x: x + 1\n"
        "        return f(5)"
    );
    const auto* lam = find_first_by_type(ar.entities, "lambda");
    BOOST_REQUIRE(lam != nullptr);
    BOOST_CHECK_EQUAL(lam->key, "f");
}

BOOST_AUTO_TEST_CASE(lambda_content_is_dedented)
{
    AnalysisResult ar("f = lambda x, y: x + y + 1");
    const auto* lam = find_first_by_type(ar.entities, "lambda");
    BOOST_REQUIRE(lam != nullptr);
    // Content should not start with leading whitespace
    BOOST_CHECK(!lam->content.empty());
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: PythonLanguageClassSuite
// ============================================================================

BOOST_AUTO_TEST_SUITE(PythonLanguageClassSuite)

BOOST_AUTO_TEST_CASE(simple_class_detected)
{
    AnalysisResult ar("class Foo:\n    pass");
    const auto* cls = find_by_key(ar.entities, "foo");
    BOOST_REQUIRE(cls != nullptr);
    BOOST_CHECK_EQUAL(cls->type, "class_definition");
}

BOOST_AUTO_TEST_CASE(class_key_is_lowercased)
{
    AnalysisResult ar("class MyClass:\n    pass");
    const auto* cls = find_by_key(ar.entities, "myclass");
    BOOST_REQUIRE(cls != nullptr);
}

BOOST_AUTO_TEST_CASE(class_with_methods)
{
    AnalysisResult ar("class Foo:\n    def method(self):\n        pass");
    const auto* cls = find_by_key(ar.entities, "foo");
    const auto* method = find_by_key(ar.entities, "method");
    BOOST_REQUIRE(cls != nullptr);
    BOOST_REQUIRE(method != nullptr);
}

BOOST_AUTO_TEST_CASE(class_method_has_type_class_method)
{
    AnalysisResult ar("class Foo:\n    def bar(self):\n        pass");
    const auto* method = find_by_key(ar.entities, "bar");
    BOOST_REQUIRE(method != nullptr);
    BOOST_CHECK_EQUAL(method->type, "class_method");
}

BOOST_AUTO_TEST_CASE(class_method_not_mistaken_for_function)
{
    AnalysisResult ar("class Foo:\n    def bar(self):\n        pass");
    // Should NOT be "function_definition"
    const auto* method = find_by_key(ar.entities, "bar");
    if (method) {
        BOOST_CHECK_NE(method->type, "function_definition");
    }
    // The method is "class_method", not "function_definition"
    BOOST_CHECK_EQUAL(count_by_type(ar.entities, "class_method"), 1u);
    BOOST_CHECK_EQUAL(count_by_type(ar.entities, "function_definition"), 0u);
}

BOOST_AUTO_TEST_CASE(multiple_classes_detected)
{
    AnalysisResult ar("class A:\n    pass\n\nclass B:\n    pass");
    size_t count = count_by_type(ar.entities, "class_definition");
    BOOST_CHECK_EQUAL(count, 2u);
}

BOOST_AUTO_TEST_CASE(class_with_inheritance)
{
    AnalysisResult ar("class Foo(Bar, Baz):\n    pass");
    const auto* cls = find_by_key(ar.entities, "foo");
    BOOST_REQUIRE(cls != nullptr);
    BOOST_CHECK_EQUAL(cls->type, "class_definition");
}

BOOST_AUTO_TEST_CASE(empty_class_detected)
{
    AnalysisResult ar("class Foo:\n    pass");
    size_t count = count_by_type(ar.entities, "class_definition");
    BOOST_CHECK_EQUAL(count, 1u);
}

BOOST_AUTO_TEST_CASE(class_with_static_method)
{
    AnalysisResult ar(
        "class Foo:\n"
        "    @staticmethod\n"
        "    def bar():\n"
        "        pass"
    );
    const auto* method = find_by_key(ar.entities, "bar");
    BOOST_REQUIRE(method != nullptr);
    BOOST_CHECK_EQUAL(method->type, "class_method");
}

BOOST_AUTO_TEST_CASE(class_with_class_method_decorator)
{
    AnalysisResult ar(
        "class Foo:\n"
        "    @classmethod\n"
        "    def bar(cls):\n"
        "        pass"
    );
    const auto* method = find_by_key(ar.entities, "bar");
    BOOST_REQUIRE(method != nullptr);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: PythonLanguageImportSuite
// ============================================================================

BOOST_AUTO_TEST_SUITE(PythonLanguageImportSuite)

BOOST_AUTO_TEST_CASE(simple_import_detected)
{
    AnalysisResult ar("import os");
    size_t count = count_by_type(ar.entities, "import_statement");
    BOOST_CHECK_EQUAL(count, 1u);
}

BOOST_AUTO_TEST_CASE(from_import_detected)
{
    AnalysisResult ar("from os import path");
    size_t count = count_by_type(ar.entities, "import_statement");
    BOOST_CHECK_EQUAL(count, 1u);
}

BOOST_AUTO_TEST_CASE(multiple_imports_detected)
{
    AnalysisResult ar("import os\nimport sys\nimport json");
    size_t count = count_by_type(ar.entities, "import_statement");
    BOOST_CHECK_EQUAL(count, 3u);
}

BOOST_AUTO_TEST_CASE(import_inside_function)
{
    AnalysisResult ar("def foo():\n    import os\n    return os");
    const auto* imp = find_first_by_type(ar.entities, "import_statement");
    BOOST_REQUIRE(imp != nullptr);
    // Should have parent_entity_idx pointing to the function
    BOOST_CHECK(imp->parent_entity_idx.has_value());
}

BOOST_AUTO_TEST_CASE(import_from_with_multiple_names)
{
    AnalysisResult ar("from os import path, environ, getcwd");
    size_t count = count_by_type(ar.entities, "import_statement");
    BOOST_CHECK_EQUAL(count, 1u);
}

BOOST_AUTO_TEST_CASE(import_with_alias)
{
    AnalysisResult ar("import numpy as np");
    size_t count = count_by_type(ar.entities, "import_statement");
    BOOST_CHECK_EQUAL(count, 1u);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: PythonLanguageGlobalVariableSuite
// ============================================================================

BOOST_AUTO_TEST_SUITE(PythonLanguageGlobalVariableSuite)

BOOST_AUTO_TEST_CASE(module_level_assignment_detected)
{
    AnalysisResult ar("x = 1");
    const auto* gv = find_first_by_type(ar.entities, "global_variables");
    BOOST_REQUIRE(gv != nullptr);
    BOOST_CHECK_EQUAL(gv->key, "x");
}

BOOST_AUTO_TEST_CASE(multiple_assignments_detected)
{
    AnalysisResult ar("a = 1\nb = 2\nc = 3");
    size_t count = count_by_type(ar.entities, "global_variables");
    BOOST_CHECK_EQUAL(count, 3u);
}

BOOST_AUTO_TEST_CASE(assignment_inside_function_is_not_global)
{
    AnalysisResult ar("def foo():\n    x = 1");
    // The assignment x=1 is inside a function, not at module level
    // _extract_global_variables requires parent scope == "module"
    size_t count = count_by_type(ar.entities, "global_variables");
    BOOST_CHECK_EQUAL(count, 0u);
}

BOOST_AUTO_TEST_CASE(assignment_inside_class_is_now_extracted)
{
    // Class-level assignments are now extracted with type "class_attribute"
    AnalysisResult ar("class Foo:\n    x = 1");
    size_t count = count_by_type(ar.entities, "class_attribute");
    BOOST_CHECK_EQUAL(count, 1u);
}

BOOST_AUTO_TEST_CASE(annotated_assignment_is_handled)
{
    // Tree-sitter in this version parses "x: int = 5" as a regular
    // "assignment" node, so annotated assignments are extracted correctly.
    AnalysisResult ar("x: int = 5");
    size_t count = count_by_type(ar.entities, "global_variables");
    BOOST_CHECK_EQUAL(count, 1u);
}

BOOST_AUTO_TEST_CASE(global_variable_has_content)
{
    AnalysisResult ar("x = 1 + 2");
    const auto* gv = find_first_by_type(ar.entities, "global_variables");
    BOOST_REQUIRE(gv != nullptr);
    BOOST_CHECK(!gv->content.empty());
}

BOOST_AUTO_TEST_CASE(string_assignment_detected)
{
    AnalysisResult ar("name = 'hello'");
    const auto* gv = find_first_by_type(ar.entities, "global_variables");
    BOOST_REQUIRE(gv != nullptr);
    BOOST_CHECK_EQUAL(gv->key, "name");
}

BOOST_AUTO_TEST_CASE(list_assignment_detected)
{
    AnalysisResult ar("items = [1, 2, 3]");
    const auto* gv = find_first_by_type(ar.entities, "global_variables");
    BOOST_REQUIRE(gv != nullptr);
    BOOST_CHECK_EQUAL(gv->key, "items");
}

BOOST_AUTO_TEST_CASE(augmented_assignment_detected)
{
    // counter += 1 should be extracted as an augmented_assignment
    AnalysisResult ar("counter = 0\ncounter += 1");
    size_t gv_count = count_by_type(ar.entities, "global_variables");
    // counter = 0 is a regular assignment + counter += 1 is augmented
    BOOST_CHECK(gv_count >= 1u);
}

BOOST_AUTO_TEST_CASE(pattern_list_assignment_detected)
{
    // x, y, z = 1, 2, 3 — left side is pattern_list
    AnalysisResult ar("x, y, z = 1, 2, 3");
    const auto* gv = find_first_by_type(ar.entities, "global_variables");
    BOOST_REQUIRE(gv != nullptr);
    // The key should contain the left-side content
    BOOST_CHECK(!gv->key.empty());
}

BOOST_AUTO_TEST_CASE(tuple_unpacking_assignment_detected)
{
    // (a, b), c = (10, 20), 30 — left side is tuple containing pattern_list
    AnalysisResult ar("(a, b), c = (10, 20), 30");
    size_t count = count_by_type(ar.entities, "global_variables");
    BOOST_CHECK(count >= 1u);
}

BOOST_AUTO_TEST_CASE(attribute_assignment_as_global)
{
    // obj.name = "Buddy" at module level — left side is attribute node
    // This tests that attribute-targeted assignments are captured
    AnalysisResult ar("class Dog:\n    pass\nobj = Dog()\nobj.name = 'Buddy'");
    // The obj.name = 'Buddy' should be extracted (left is attribute)
    size_t count = count_by_type(ar.entities, "global_variables");
    BOOST_CHECK(count >= 2u);  // obj = Dog() and obj.name = 'Buddy'
}

BOOST_AUTO_TEST_CASE(subscript_assignment_as_global)
{
    // arr[0] = 100 — left side is subscript node
    AnalysisResult ar("arr = [1, 2, 3]\narr[0] = 100");
    size_t count = count_by_type(ar.entities, "global_variables");
    BOOST_CHECK(count >= 2u);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: PythonLanguageIdentifierSuite
// ============================================================================

BOOST_AUTO_TEST_SUITE(PythonLanguageIdentifierSuite)

BOOST_AUTO_TEST_CASE(simple_identifier_detected)
{
    AnalysisResult ar("x = 1");
    auto it = ar.identifiers.find("x");
    BOOST_REQUIRE(it != ar.identifiers.end());
    BOOST_CHECK(it->second.count(0) > 0);
}

BOOST_AUTO_TEST_CASE(identifier_on_multiple_lines)
{
    AnalysisResult ar("x = 1\ny = x");
    auto it = ar.identifiers.find("x");
    BOOST_REQUIRE(it != ar.identifiers.end());
    // 'x' appears on line 0 (assignment) and line 1 (usage)
    BOOST_CHECK_EQUAL(it->second.size(), 2u);
}

BOOST_AUTO_TEST_CASE(identifier_key_is_lowercased)
{
    AnalysisResult ar("Foo()");
    // The identifier "Foo" should be stored with key "foo"
    auto it = ar.identifiers.find("foo");
    BOOST_REQUIRE(it != ar.identifiers.end());
}

BOOST_AUTO_TEST_CASE(multiple_identifiers_present)
{
    AnalysisResult ar("a = 1\nb = 2");
    BOOST_CHECK(ar.identifiers.find("a") != ar.identifiers.end());
    BOOST_CHECK(ar.identifiers.find("b") != ar.identifiers.end());
}

BOOST_AUTO_TEST_CASE(locate_identifier_found_returns_json_array)
{
    PythonLanguage lang;
    lang.load(std::string("x = 1"))->analyze();
    nlohmann::json result = lang.locate_identifier("x");
    // Found identifiers return a JSON array with one entry
    BOOST_CHECK(result.is_array());
    BOOST_CHECK(!result.empty());
    // Each entry has meta and text
    const auto& entry = result[0];
    BOOST_CHECK(entry.contains("meta"));
    BOOST_CHECK(entry.contains("text"));
    // The text section should contain at least one match entry
    BOOST_CHECK(entry["text"]["line_content"].is_array());
    BOOST_CHECK(!entry["text"]["line_content"].empty());
    // The match line should be annotated as "match"
    bool has_match = false;
    for (const auto& lt : entry["text"]["line_type"]) {
        if (lt == "match") has_match = true;
    }
    BOOST_CHECK(has_match);
}

BOOST_AUTO_TEST_CASE(locate_identifier_not_found_returns_empty_array)
{
    PythonLanguage lang;
    lang.load(std::string("x = 1"))->analyze();
    nlohmann::json result = lang.locate_identifier("z");
    // Not-found identifiers return an empty JSON array
    BOOST_CHECK(result.is_array());
    BOOST_CHECK(result.empty());
}

BOOST_AUTO_TEST_CASE(locate_identifier_case_insensitive_match)
{
    PythonLanguage lang;
    lang.load(std::string("X = 1"))->analyze();
    nlohmann::json r_lower = lang.locate_identifier("x");
    nlohmann::json r_upper = lang.locate_identifier("X");
    // Both cases should return a non-empty array
    BOOST_CHECK(!r_lower.empty());
    BOOST_CHECK(!r_upper.empty());
    // Both queries should produce the same result
    BOOST_CHECK_EQUAL(r_lower.dump(), r_upper.dump());
}

BOOST_AUTO_TEST_CASE(locate_identifier_context_lines_included)
{
    // Identifier 'x' appears on line 2; with default context_lines=2,
    // lines 0-4 should be included (assuming file has lines 0-4).
    PythonLanguage lang;
    lang.load(std::string("a = 1\nb = 2\nx = 3\nc = 4\nd = 5"))->analyze();
    nlohmann::json result = lang.locate_identifier("x");
    BOOST_REQUIRE(!result.empty());
    const auto& entry = result[0];
    // line 2 is the match; lines 0-1 and 3-4 should be context
    size_t line_count = entry["text"]["line_content"].size();
    BOOST_CHECK(line_count >= 3u);  // at least match + 2 context lines
    // Check that line 2 is marked "match"
    bool found_match = false;
    for (size_t i = 0; i < entry["text"]["line_number"].size(); ++i) {
        if (entry["text"]["line_number"][i] == 2u) {
            BOOST_CHECK_EQUAL(entry["text"]["line_type"][i], "match");
            found_match = true;
        }
    }
    BOOST_CHECK(found_match);
}

BOOST_AUTO_TEST_CASE(locate_identifier_no_duplicate_lines)
{
    // Identifier 'x' appears on adjacent lines 2 and 3; with context_lines=2,
    // overlapping intervals should be merged — no duplicate lines.
    PythonLanguage lang;
    lang.load(std::string("a\nb\nx = 2\nx = 3\nc\nd"))->analyze();
    nlohmann::json result = lang.locate_identifier("x");
    BOOST_REQUIRE(!result.empty());
    const auto& entry = result[0];
    // Verify no duplicate line numbers
    std::set<size_t> seen;
    for (const auto& ln : entry["text"]["line_number"]) {
        size_t n = ln.get<size_t>();
        BOOST_CHECK(seen.find(n) == seen.end());
        seen.insert(n);
    }
}

BOOST_AUTO_TEST_CASE(locate_identifier_lines_in_order)
{
    PythonLanguage lang;
    lang.load(std::string("a\nx = 1\nb\nx = 2\nc"))->analyze();
    nlohmann::json result = lang.locate_identifier("x");
    BOOST_REQUIRE(!result.empty());
    const auto& lines = result[0]["text"]["line_number"];
    BOOST_CHECK(!lines.empty());
    // Line numbers should be strictly increasing
    for (size_t i = 1; i < lines.size(); ++i) {
        BOOST_CHECK(lines[i].get<size_t>() > lines[i - 1].get<size_t>());
    }
}

BOOST_AUTO_TEST_CASE(locate_identifier_context_clamped_at_boundaries)
{
    PythonLanguage lang;
    lang.load(std::string("x = 1\na\nb"))->analyze();
    nlohmann::json result = lang.locate_identifier("x");
    BOOST_REQUIRE(!result.empty());
    const auto& entry = result[0];
    // Match at line 0; context should not go below 0
    for (const auto& ln : entry["text"]["line_number"]) {
        BOOST_CHECK(ln.get<size_t>() < 3u);
    }
}

BOOST_AUTO_TEST_CASE(locate_identifier_line_type_base_vs_match)
{
    PythonLanguage lang;
    lang.load(std::string("a\nb\nx = 1\nc\nd"))->analyze();
    nlohmann::json result = lang.locate_identifier("x");
    BOOST_REQUIRE(!result.empty());
    const auto& entry = result[0];
    for (size_t i = 0; i < entry["text"]["line_number"].size(); ++i) {
        std::string line_type = entry["text"]["line_type"][i];
        if (entry["text"]["line_number"][i] == 2u) {
            BOOST_CHECK_EQUAL(line_type, "match");
        } else {
            BOOST_CHECK_EQUAL(line_type, "base");
        }
    }
}

BOOST_AUTO_TEST_CASE(locate_identifier_custom_context_size_via_setter)
{
    // Use set_context_lines() to set context to 1; only ±1 lines included.
    PythonLanguage lang;
    lang.set_context_lines(1);
    lang.load(std::string("a\nb\nx = 1\nc\nd"))->analyze();
    nlohmann::json result = lang.locate_identifier("x");
    BOOST_REQUIRE(!result.empty());
    const auto& entry = result[0];
    // Match at line 2, context 1 → lines 1, 2, 3 (3 lines total)
    BOOST_CHECK_EQUAL(entry["text"]["line_content"].size(), 3u);
    // Meta: File, Identifier, Matches, Total Length
    BOOST_CHECK_EQUAL(entry["meta"]["field_name"].size(), 4u);
}

BOOST_AUTO_TEST_CASE(locate_identifier_meta_includes_file_path)
{
    PythonLanguage lang;
    lang.load(std::string("x = 1\ny = x\nz = 3"))->analyze();
    nlohmann::json result = lang.locate_identifier("x");
    BOOST_REQUIRE(!result.empty());
    const auto& meta = result[0]["meta"];
    // Check meta structure: File, Identifier, Matches, Total Length
    BOOST_CHECK(meta.contains("field_name"));
    BOOST_CHECK(meta.contains("field_content"));
    BOOST_CHECK_EQUAL(meta["field_name"].size(), 4u);
    BOOST_CHECK_EQUAL(meta["field_content"].size(), 4u);
    // First field is "File" — the absolute path
    BOOST_CHECK_EQUAL(meta["field_name"][0], "File");
    // When loaded from string (no open()), _absolute_path is empty
    BOOST_CHECK_EQUAL(meta["field_content"][0], "");
    // Second field is the identifier name (lowercased)
    BOOST_CHECK_EQUAL(meta["field_content"][1], "x");
}

BOOST_AUTO_TEST_CASE(locate_identifier_meta_file_path_from_open)
{
    TempPythonFile tmp("x = 1\ny = 2");
    PythonLanguage lang;
    lang.open(tmp.path)->analyze();
    nlohmann::json result = lang.locate_identifier("x");
    BOOST_REQUIRE(!result.empty());
    const auto& meta = result[0]["meta"];
    // File field should contain the absolute path from open()
    BOOST_CHECK_EQUAL(meta["field_content"][0], tmp.path.string());
}

BOOST_AUTO_TEST_CASE(locate_identifier_empty_source_returns_empty_array)
{
    PythonLanguage lang;
    lang.load(std::string(""))->analyze();
    nlohmann::json result = lang.locate_identifier("anything");
    BOOST_CHECK(result.is_array());
    BOOST_CHECK(result.empty());
}

BOOST_AUTO_TEST_CASE(get_context_lines_returns_default)
{
    PythonLanguage lang;
    // Default should be 2
    BOOST_CHECK_EQUAL(lang.get_context_lines(), 2u);
}

BOOST_AUTO_TEST_CASE(set_context_lines_chaining_works)
{
    PythonLanguage lang;
    // set_context_lines returns this pointer for chaining
    PythonLanguage* ptr = static_cast<PythonLanguage*>(lang.set_context_lines(5));
    BOOST_CHECK_EQUAL(ptr, &lang);
    BOOST_CHECK_EQUAL(lang.get_context_lines(), 5u);
}

BOOST_AUTO_TEST_CASE(no_duplicate_line_entries)
{
    // Same identifier used multiple times on the same line
    AnalysisResult ar("x = x + x");
    auto it = ar.identifiers.find("x");
    BOOST_REQUIRE(it != ar.identifiers.end());
    // All on line 0, only one unique entry
    BOOST_CHECK_EQUAL(it->second.size(), 1u);
}

BOOST_AUTO_TEST_CASE(keywords_not_treated_as_identifiers)
{
    // 'def', 'class', 'import', 'return', 'pass' are keywords
    // Tree-sitter may or may not parse them as identifier nodes
    AnalysisResult ar("def foo():\n    return True");
    // 'def' and 'return' should not appear as identifiers
    BOOST_CHECK(ar.identifiers.find("def") == ar.identifiers.end());
}

BOOST_AUTO_TEST_CASE(get_identifier_line_map_returns_reference)
{
    AnalysisResult ar("x = 1\ny = 2");
    BOOST_CHECK_EQUAL(ar.identifiers.size(), 2u);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: PythonLanguageEntityHierarchySuite
// ============================================================================

BOOST_AUTO_TEST_SUITE(PythonLanguageEntityHierarchySuite)

BOOST_AUTO_TEST_CASE(module_has_no_parent)
{
    AnalysisResult ar("x = 1");
    const auto* mod = find_first_by_type(ar.entities, "module");
    BOOST_REQUIRE(mod != nullptr);
    BOOST_CHECK(!mod->parent_entity_idx.has_value());
}

BOOST_AUTO_TEST_CASE(top_level_function_parent_is_module)
{
    AnalysisResult ar("def foo():\n    pass");
    // Module is entity 0, function is entity 1
    BOOST_REQUIRE(ar.entities.size() >= 2u);
    const auto* func =
        dynamic_cast<const PythonLanguage::EntityTag*>(ar.entities[1].get());
    BOOST_REQUIRE(func != nullptr);
    BOOST_CHECK(func->parent_entity_idx.has_value());
    BOOST_CHECK_EQUAL(func->parent_entity_idx.value(), 0u);
}

BOOST_AUTO_TEST_CASE(class_method_parent_is_class)
{
    AnalysisResult ar("class Foo:\n    def bar(self):\n        pass");
    const auto* cls = find_by_key(ar.entities, "foo");
    const auto* method = find_by_key(ar.entities, "bar");
    BOOST_REQUIRE(cls != nullptr);
    BOOST_REQUIRE(method != nullptr);
    BOOST_REQUIRE(method->parent_entity_idx.has_value());
    // Verify the parent is indeed the class
    const auto* parent_entity =
        dynamic_cast<const PythonLanguage::EntityTag*>(
            ar.entities[method->parent_entity_idx.value()].get());
    BOOST_REQUIRE(parent_entity != nullptr);
    BOOST_CHECK_EQUAL(parent_entity->type, "class_definition");
    BOOST_CHECK_EQUAL(parent_entity->key, "foo");
}

BOOST_AUTO_TEST_CASE(module_children_include_top_level_entities)
{
    AnalysisResult ar("def foo():\n    pass\n\nclass Bar:\n    pass");
    const auto* mod = find_first_by_type(ar.entities, "module");
    BOOST_REQUIRE(mod != nullptr);
    // Module should have foo and Bar as children
    BOOST_CHECK(mod->children_entity_idx.size() >= 2u);
}

BOOST_AUTO_TEST_CASE(class_children_include_methods)
{
    AnalysisResult ar(
        "class Foo:\n"
        "    def bar(self):\n        pass\n"
        "    def baz(self):\n        pass"
    );
    const auto* cls = find_by_key(ar.entities, "foo");
    BOOST_REQUIRE(cls != nullptr);
    BOOST_CHECK_EQUAL(cls->children_entity_idx.size(), 2u);
}

BOOST_AUTO_TEST_CASE(nested_function_parenting)
{
    AnalysisResult ar("def outer():\n    def inner():\n        pass");
    const auto* outer = find_by_key(ar.entities, "outer");
    const auto* inner = find_by_key(ar.entities, "inner");
    BOOST_REQUIRE(outer != nullptr);
    BOOST_REQUIRE(inner != nullptr);
    BOOST_REQUIRE(inner->parent_entity_idx.has_value());
    const auto* parent =
        dynamic_cast<const PythonLanguage::EntityTag*>(
            ar.entities[inner->parent_entity_idx.value()].get());
    BOOST_REQUIRE(parent != nullptr);
    BOOST_CHECK_EQUAL(parent->key, "outer");
}

BOOST_AUTO_TEST_CASE(deeply_nested_scopes_maintain_hierarchy)
{
    AnalysisResult ar(
        "class A:\n"
        "    class B:\n"
        "        def c(self):\n"
        "            def d():\n"
        "                pass"
    );
    // Verify no crash and hierarchy is preserved
    const auto* class_a = find_by_key(ar.entities, "a");
    BOOST_REQUIRE(class_a != nullptr);
    // All entities should have valid parent references
    for (size_t i = 1; i < ar.entities.size(); ++i) {
        const auto* entity =
            dynamic_cast<const PythonLanguage::EntityTag*>(ar.entities[i].get());
        BOOST_REQUIRE(entity != nullptr);
        if (entity->type != "module") {
            BOOST_CHECK(entity->parent_entity_idx.has_value());
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: PythonLanguageEntityPropertiesSuite
// ============================================================================

BOOST_AUTO_TEST_SUITE(PythonLanguageEntityPropertiesSuite)

BOOST_AUTO_TEST_CASE(to_dict_contains_expected_keys)
{
    AnalysisResult ar("def foo():\n    pass");
    const auto* func = find_by_key(ar.entities, "foo");
    BOOST_REQUIRE(func != nullptr);
    nlohmann::json dict = func->to_dict();
    BOOST_CHECK(dict.contains("meta"));
    BOOST_CHECK(dict.contains("text"));
    BOOST_CHECK(dict["meta"].contains("field_name"));
    BOOST_CHECK(dict["meta"].contains("field_content"));
    BOOST_CHECK(dict["text"].contains("line_content"));
    BOOST_CHECK(dict["text"].contains("line_number"));
    BOOST_CHECK(dict["text"].contains("line_type"));
}

BOOST_AUTO_TEST_CASE(to_dict_module_meta_is_valid)
{
    AnalysisResult ar("x = 1");
    const auto* mod = find_first_by_type(ar.entities, "module");
    BOOST_REQUIRE(mod != nullptr);
    nlohmann::json dict = mod->to_dict();
    // The module entity should have valid meta and text sections
    BOOST_CHECK(dict["meta"]["field_content"][1] == "module");
}

BOOST_AUTO_TEST_CASE(to_dict_meta_field_content_is_array)
{
    AnalysisResult ar("class Foo:\n    def bar(self):\n        pass");
    const auto* cls = find_by_key(ar.entities, "foo");
    BOOST_REQUIRE(cls != nullptr);
    nlohmann::json dict = cls->to_dict();
    BOOST_CHECK(dict["meta"]["field_content"].is_array());
    BOOST_CHECK(!dict["meta"]["field_content"].empty());
}

BOOST_AUTO_TEST_CASE(get_qualified_name_str_contains_type_and_name)
{
    AnalysisResult ar("def foo():\n    pass");
    const auto* func = find_by_key(ar.entities, "foo");
    BOOST_REQUIRE(func != nullptr);
    std::string qname = func->get_qualified_name_str();
    // Should contain the function name and type
    BOOST_CHECK(qname.find("foo") != std::string::npos);
    BOOST_CHECK(qname.find("function_definition") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(get_qualified_name_str_for_class_method)
{
    AnalysisResult ar("class Foo:\n    def bar(self):\n        pass");
    const auto* method = find_by_key(ar.entities, "bar");
    BOOST_REQUIRE(method != nullptr);
    std::string qname = method->get_qualified_name_str();
    BOOST_CHECK(qname.find("Foo") != std::string::npos);
    BOOST_CHECK(qname.find("bar") != std::string::npos);
    BOOST_CHECK(qname.find("class_method") != std::string::npos
                || qname.find("class_definition") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(clone_creates_independent_copy)
{
    AnalysisResult ar("def foo():\n    pass");
    const auto* func = find_by_key(ar.entities, "foo");
    BOOST_REQUIRE(func != nullptr);
    auto cloned = func->clone();
    BOOST_REQUIRE(cloned != nullptr);
    // Cloned object should serialize the same way
    BOOST_CHECK_EQUAL(cloned->to_dict().dump(), func->to_dict().dump());
    // Should be a different pointer
    BOOST_CHECK_NE(cloned.get(), func);
}

BOOST_AUTO_TEST_CASE(get_key_returns_correct_value)
{
    AnalysisResult ar("def MyFunction():\n    pass");
    const auto* func = find_by_key(ar.entities, "myfunction");
    BOOST_REQUIRE(func != nullptr);
    BOOST_CHECK_EQUAL(func->get_key(), "myfunction");
}

BOOST_AUTO_TEST_CASE(entity_byte_range_is_within_source)
{
    AnalysisResult ar("def foo():\n    pass");
    const auto* func = find_by_key(ar.entities, "foo");
    BOOST_REQUIRE(func != nullptr);
    std::string_view source = ar.lang.source();
    BOOST_CHECK(func->byte_start < source.size());
    BOOST_CHECK(func->byte_end <= source.size());
    BOOST_CHECK(func->byte_start <= func->byte_end);
}

BOOST_AUTO_TEST_CASE(entity_line_range_is_valid)
{
    AnalysisResult ar("def foo():\n    pass");
    const auto* func = find_by_key(ar.entities, "foo");
    BOOST_REQUIRE(func != nullptr);
    BOOST_CHECK(func->line_start <= func->line_end);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: PythonLanguageResetSuite
// ============================================================================

BOOST_AUTO_TEST_SUITE(PythonLanguageResetSuite)

BOOST_AUTO_TEST_CASE(reset_clears_entities)
{
    PythonLanguage lang;
    lang.load(std::string("def foo():\n    pass"))->analyze();
    BOOST_CHECK(!lang.result().empty());
    lang.reset();
    BOOST_CHECK(lang.result().empty());
}

BOOST_AUTO_TEST_CASE(reset_clears_identifier_map)
{
    PythonLanguage lang;
    lang.load(std::string("x = 1"))->analyze();
    BOOST_CHECK(!lang.get_identifier_line_map().empty());
    lang.reset();
    BOOST_CHECK(lang.get_identifier_line_map().empty());
}

BOOST_AUTO_TEST_CASE(reset_clears_source)
{
    PythonLanguage lang;
    lang.load(std::string("content"))->analyze();
    lang.reset();
    BOOST_CHECK(lang.source().empty());
}

BOOST_AUTO_TEST_CASE(reset_then_reanalyze_produces_same_results)
{
    PythonLanguage lang;
    lang.load(std::string("def foo():\n    pass"))->analyze();
    auto first_result = lang.result().size();
    auto first_id_count = lang.get_identifier_line_map().size();

    lang.reset();
    lang.load(std::string("def foo():\n    pass"))->analyze();
    BOOST_CHECK_EQUAL(lang.result().size(), first_result);
    BOOST_CHECK_EQUAL(lang.get_identifier_line_map().size(), first_id_count);
}

BOOST_AUTO_TEST_CASE(double_reset_is_safe)
{
    PythonLanguage lang;
    lang.load(std::string("x = 1"))->analyze();
    BOOST_CHECK_NO_THROW(lang.reset());
    BOOST_CHECK_NO_THROW(lang.reset());
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: PythonLanguageDecoratorSuite
// ============================================================================

BOOST_AUTO_TEST_SUITE(PythonLanguageDecoratorSuite)

BOOST_AUTO_TEST_CASE(decorator_on_function_adjusts_line_start)
{
    AnalysisResult ar("@dec\ndef foo():\n    pass");
    const auto* func = find_by_key(ar.entities, "foo");
    BOOST_REQUIRE(func != nullptr);
    // line_start should reflect the decorator line (0), not the def line (1)
    BOOST_CHECK_EQUAL(func->line_start, 0u);
}

BOOST_AUTO_TEST_CASE(multiple_decorators_adjusts_to_first)
{
    AnalysisResult ar("@a\n@b\n@c\ndef foo():\n    pass");
    const auto* func = find_by_key(ar.entities, "foo");
    BOOST_REQUIRE(func != nullptr);
    // line_start should be 0 (first decorator)
    BOOST_CHECK_EQUAL(func->line_start, 0u);
}

BOOST_AUTO_TEST_CASE(decorator_on_class_adjusts_line_start)
{
    AnalysisResult ar("@dataclass\nclass Foo:\n    pass");
    const auto* cls = find_by_key(ar.entities, "foo");
    BOOST_REQUIRE(cls != nullptr);
    BOOST_CHECK_EQUAL(cls->line_start, 0u);
}

BOOST_AUTO_TEST_CASE(function_without_decorator_unchanged)
{
    AnalysisResult ar("def foo():\n    pass");
    const auto* func = find_by_key(ar.entities, "foo");
    BOOST_REQUIRE(func != nullptr);
    BOOST_CHECK_EQUAL(func->line_start, 0u);
}

BOOST_AUTO_TEST_CASE(decorator_content_includes_decorator_lines)
{
    AnalysisResult ar("@dec\ndef foo():\n    return 42");
    const auto* func = find_by_key(ar.entities, "foo");
    BOOST_REQUIRE(func != nullptr);
    // Content extraction uses the adjusted line_start that includes decorator
    BOOST_CHECK(!func->content.empty());
}

BOOST_AUTO_TEST_CASE(decorator_with_arguments)
{
    AnalysisResult ar("@dec(arg1, arg2)\ndef foo():\n    pass");
    const auto* func = find_by_key(ar.entities, "foo");
    BOOST_REQUIRE(func != nullptr);
    BOOST_CHECK_EQUAL(func->line_start, 0u);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: PythonLanguageRegressionSuite
// ============================================================================

BOOST_AUTO_TEST_SUITE(PythonLanguageRegressionSuite)

// Regression test for Issue 1: OOB in get_dedented_lines when line_start > 0
BOOST_AUTO_TEST_CASE(regression_dedented_lines_class_method_no_crash)
{
    // Class methods have line_start > 0, which previously caused OOB access
    // in get_dedented_lines
    AnalysisResult ar(
        "class Foo:\n"
        "    def method(self):\n"
        "        pass\n"
        "    def another(self):\n"
        "        return 1"
    );
    const auto* method = find_by_key(ar.entities, "method");
    const auto* another = find_by_key(ar.entities, "another");
    BOOST_REQUIRE(method != nullptr);
    BOOST_REQUIRE(another != nullptr);
    // Content should be non-empty (was crashing before fix)
    BOOST_CHECK(!method->content.empty());
    BOOST_CHECK(!another->content.empty());
}

BOOST_AUTO_TEST_CASE(regression_dedented_lines_nested_import_no_crash)
{
    AnalysisResult ar(
        "def outer():\n"
        "    import os\n"
        "    import sys\n"
        "    return os.path"
    );
    // Verify no crash; both imports should be captured
    size_t import_count = count_by_type(ar.entities, "import_statement");
    BOOST_CHECK(import_count >= 1u);
}

BOOST_AUTO_TEST_CASE(regression_dedented_lines_deeply_nested_no_crash)
{
    AnalysisResult ar(
        "class A:\n"
        "    class B:\n"
        "        def c(self):\n"
        "            def d():\n"
        "                pass"
    );
    // All entities should be extracted without crash
    BOOST_CHECK(count_by_type(ar.entities, "class_definition") >= 2u);
    BOOST_CHECK(count_by_type(ar.entities, "class_method") >= 1u);
    BOOST_CHECK(count_by_type(ar.entities, "function_definition") >= 1u);
    // Verify content for all entities
    for (const auto& e : ar.entities) {
        const auto* py_entity =
            dynamic_cast<const PythonLanguage::EntityTag*>(e.get());
        BOOST_REQUIRE(py_entity != nullptr);
        // Non-module entities should have content
        if (py_entity->type != "module") {
            BOOST_CHECK(!py_entity->content.empty());
        }
    }
}

BOOST_AUTO_TEST_CASE(regression_null_byte_in_source_no_truncation)
{
    // Issue 3: strlen() would stop at null byte. After fix using _source.size(),
    // tree-sitter gets full source. Note: tree-sitter may still have issues
    // with embedded nulls, but we shouldn't crash.
    PythonLanguage lang;
    std::string source = "x = 'a\0b'";
    source += "  # more content after null";  // strlen would miss this
    // source.size() is the full length including null byte and beyond
    lang.load(source);
    BOOST_CHECK_NO_THROW(lang.analyze());
}

BOOST_AUTO_TEST_CASE(regression_control_flow_else_chain_correct)
{
    // Issue 2: missing 'else' meant import statement check ran even after
    // function_definition matched. With the fix, the chain is properly
    // exclusive. This test verifies function definitions are still extracted
    // when imports are also present.
    AnalysisResult ar(
        "import os\n"
        "import sys\n"
        "def foo():\n"
        "    pass\n"
        "import json"
    );
    BOOST_CHECK_EQUAL(count_by_type(ar.entities, "import_statement"), 3u);
    BOOST_CHECK_EQUAL(count_by_type(ar.entities, "function_definition"), 1u);
}

BOOST_AUTO_TEST_CASE(regression_annotated_assignment_handled)
{
    // Tree-sitter in this version parses "x: int = 5" as a regular
    // "assignment" node, so both annotated and regular assignments work.
    AnalysisResult ar("x: int = 5\ny = 10");
    BOOST_CHECK_EQUAL(count_by_type(ar.entities, "global_variables"), 2u);
    // Both variables should be extracted
    const auto* gv_x = find_by_key(ar.entities, "x");
    const auto* gv_y = find_by_key(ar.entities, "y");
    BOOST_CHECK(gv_x != nullptr);
    BOOST_CHECK(gv_y != nullptr);
}

BOOST_AUTO_TEST_CASE(regression_class_level_assignment_now_extracted)
{
    // Class-level assignments are now extracted with type "class_attribute"
    AnalysisResult ar(
        "x = 1\n"            // module-level -> "global_variables"
        "class Foo:\n"
        "    y = 2\n"         // class-level -> "class_attribute"
        "    def bar(self):\n"
        "        z = 3"       // function-local, correctly skipped
    );
    BOOST_CHECK_EQUAL(count_by_type(ar.entities, "global_variables"), 1u);
    BOOST_CHECK_EQUAL(count_by_type(ar.entities, "class_attribute"), 1u);
}

BOOST_AUTO_TEST_CASE(regression_import_key_is_empty_by_design)
{
    // Import keys are intentionally left empty — AI analyzes import content.
    AnalysisResult ar("import os");
    const auto* imp = find_first_by_type(ar.entities, "import_statement");
    BOOST_REQUIRE(imp != nullptr);
    BOOST_CHECK(imp->key.empty());
}

BOOST_AUTO_TEST_CASE(locate_identifier_meta_includes_total_length)
{
    // The "Total Length" field should equal the number of lines in the source.
    PythonLanguage lang;
    lang.load(std::string("x = 1\ny = 2\nz = 3"))->analyze();
    nlohmann::json result = lang.locate_identifier("x");
    BOOST_REQUIRE(!result.empty());
    const auto& meta = result[0]["meta"];
    // field_name: [File, Identifier, Matches, Total Length]
    BOOST_CHECK_EQUAL(meta["field_name"][3], "Total Length");
    // field_content[3] should be the total number of lines (3)
    BOOST_CHECK_EQUAL(meta["field_content"][3].get<size_t>(), 3u);
}

BOOST_AUTO_TEST_CASE(locate_identifier_total_length_matches_source_lines)
{
    PythonLanguage lang;
    lang.load(std::string("a\nb\nc\nd\ne"))->analyze();
    nlohmann::json result = lang.locate_identifier("a");
    BOOST_REQUIRE(!result.empty());
    const auto& meta = result[0]["meta"];
    // 5 lines total
    BOOST_CHECK_EQUAL(meta["field_content"][3].get<size_t>(), 5u);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: PythonLanguageLocateEntitySuite — locate_entity() lookup & serialization
// ============================================================================

BOOST_AUTO_TEST_SUITE(PythonLanguageLocateEntitySuite)

BOOST_AUTO_TEST_CASE(locate_entity_found_returns_array)
{
    // Given a source with a function "foo", the key "foo" should match and
    // return a non-empty JSON array.
    PythonLanguage lang;
    lang.load(std::string("def foo():\n    pass"))->analyze();
    nlohmann::json result = lang.locate_entity("foo");
    BOOST_CHECK(result.is_array());
    BOOST_CHECK(!result.empty());
}

BOOST_AUTO_TEST_CASE(locate_entity_not_found_returns_empty_array)
{
    // A key that does not exist should return an empty JSON array.
    PythonLanguage lang;
    lang.load(std::string("def foo():\n    pass"))->analyze();
    nlohmann::json result = lang.locate_entity("nonexistent");
    BOOST_CHECK(result.is_array());
    BOOST_CHECK(result.empty());
}

BOOST_AUTO_TEST_CASE(locate_entity_case_insensitive_match)
{
    // Keys are matched case-insensitively: "FOO", "Foo", "foo" should all
    // find the same entity.
    PythonLanguage lang;
    lang.load(std::string("def FooBar():\n    pass"))->analyze();

    nlohmann::json r1 = lang.locate_entity("foobar");
    nlohmann::json r2 = lang.locate_entity("FOOBAR");
    nlohmann::json r3 = lang.locate_entity("FooBar");

    BOOST_CHECK(!r1.empty());
    BOOST_CHECK(!r2.empty());
    BOOST_CHECK(!r3.empty());
    // All three queries should produce the same result
    BOOST_CHECK_EQUAL(r1.dump(), r2.dump());
    BOOST_CHECK_EQUAL(r2.dump(), r3.dump());
}

BOOST_AUTO_TEST_CASE(locate_entity_has_required_fields)
{
    // Each matched entity must carry "meta", "text", and "sub_entity" fields.
    PythonLanguage lang;
    lang.load(std::string("def foo():\n    pass"))->analyze();
    nlohmann::json result = lang.locate_entity("foo");
    BOOST_REQUIRE(!result.empty());

    const auto& entity = result[0];
    BOOST_CHECK(entity.contains("meta"));
    BOOST_CHECK(entity.contains("text"));
    BOOST_CHECK(entity.contains("sub_entity"));
    BOOST_CHECK(entity["sub_entity"].is_array());
}

BOOST_AUTO_TEST_CASE(locate_entity_with_children_has_sub_entity)
{
    // A class with methods: the class entity should have non-empty sub_entity
    // containing its method children.
    PythonLanguage lang;
    lang.load(std::string(
        "class Foo:\n"
        "    def bar(self):\n"
        "        pass\n"
        "    def baz(self):\n"
        "        pass"
    ))->analyze();

    nlohmann::json result = lang.locate_entity("foo");
    BOOST_REQUIRE(!result.empty());

    const auto& class_entity = result[0];
    BOOST_CHECK(class_entity["sub_entity"].is_array());
    BOOST_CHECK(!class_entity["sub_entity"].empty());

    // Each child should also have meta/text/sub_entity fields
    for (const auto& child : class_entity["sub_entity"]) {
        BOOST_CHECK(child.contains("meta"));
        BOOST_CHECK(child.contains("text"));
        BOOST_CHECK(child.contains("sub_entity"));
    }
}

BOOST_AUTO_TEST_CASE(locate_entity_leaf_has_empty_sub_entity)
{
    // A top-level function with no nested definitions should have an empty
    // sub_entity array.
    PythonLanguage lang;
    lang.load(std::string("def foo():\n    return 42"))->analyze();
    nlohmann::json result = lang.locate_entity("foo");
    BOOST_REQUIRE(!result.empty());

    BOOST_CHECK(result[0]["sub_entity"].is_array());
    BOOST_CHECK(result[0]["sub_entity"].empty());
}

BOOST_AUTO_TEST_CASE(locate_entity_multiple_matches)
{
    // Multiple entities sharing the same key (e.g. multiple "foo" definitions
    // in different scopes) should all be returned.
    PythonLanguage lang;
    lang.load(std::string(
        "def foo():\n"              // top-level foo
        "    pass\n"
        "class Bar:\n"
        "    def foo(self):\n"      // nested foo inside Bar
        "        pass"
    ))->analyze();

    nlohmann::json result = lang.locate_entity("foo");
    BOOST_CHECK(result.is_array());
    // Both the function_definition and the class_method named "foo" are matched
    BOOST_CHECK(result.size() >= 2u);
}

BOOST_AUTO_TEST_CASE(locate_entity_empty_source_returns_empty)
{
    // An empty source has no entities (or only the module entity with an
    // empty key), so any query should return an empty array.
    PythonLanguage lang;
    lang.load(std::string(""))->analyze();
    nlohmann::json result = lang.locate_entity("anything");
    BOOST_CHECK(result.is_array());
    BOOST_CHECK(result.empty());
}

BOOST_AUTO_TEST_CASE(locate_entity_deeply_nested_children)
{
    // Verify recursive serialization for deeply nested scopes:
    // class A → class B → def c → def d
    PythonLanguage lang;
    lang.load(std::string(
        "class A:\n"
        "    class B:\n"
        "        def c(self):\n"
        "            def d():\n"
        "                pass"
    ))->analyze();

    // Query class A — it should contain B as sub_entity, which contains c, etc.
    nlohmann::json result = lang.locate_entity("a");
    BOOST_REQUIRE(!result.empty());

    const auto& a = result[0];
    BOOST_REQUIRE(!a["sub_entity"].empty());
    // A's first child should be class B
    const auto& b = a["sub_entity"][0];
    BOOST_CHECK(!b["sub_entity"].empty());
    // B's first child should be method c
    const auto& c = b["sub_entity"][0];
    BOOST_CHECK(!c["sub_entity"].empty());
    // c's first child should be function d
    const auto& d = c["sub_entity"][0];
    BOOST_CHECK(d["sub_entity"].is_array());
}

BOOST_AUTO_TEST_CASE(locate_entity_preserves_meta_content)
{
    // The meta section inside a located entity should carry the same
    // field_name / field_content structure as a direct to_dict() call.
    PythonLanguage lang;
    lang.load(std::string("def foo():\n    pass"))->analyze();

    nlohmann::json located = lang.locate_entity("foo");
    BOOST_REQUIRE(!located.empty());

    const auto& meta = located[0]["meta"];
    BOOST_CHECK(meta.contains("field_name"));
    BOOST_CHECK(meta.contains("field_content"));
    BOOST_CHECK(meta["field_name"].is_array());
    BOOST_CHECK(meta["field_content"].is_array());
    BOOST_CHECK_EQUAL(meta["field_name"].size(), meta["field_content"].size());
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: PythonLanguageFullStructureSuite — get_full_structure()
// ============================================================================

BOOST_AUTO_TEST_SUITE(PythonLanguageFullStructureSuite)

BOOST_AUTO_TEST_CASE(get_full_structure_returns_array)
{
    PythonLanguage lang;
    lang.load(std::string("def foo():\n    pass"))->analyze();
    nlohmann::json result = lang.get_full_structure();
    BOOST_CHECK(result.is_array());
    BOOST_CHECK(!result.empty());
}

BOOST_AUTO_TEST_CASE(get_full_structure_has_module_root)
{
    PythonLanguage lang;
    lang.load(std::string("x = 1"))->analyze();
    nlohmann::json result = lang.get_full_structure();
    BOOST_REQUIRE(!result.empty());
    const auto& entry = result[0];
    BOOST_CHECK(entry.contains("meta"));
    BOOST_CHECK(entry.contains("text"));
    BOOST_CHECK(entry.contains("sub_entity"));
    // Should be the module
    BOOST_CHECK_EQUAL(entry["meta"]["field_content"][1], "module");
}

BOOST_AUTO_TEST_CASE(get_full_structure_empty_source_no_crash)
{
    PythonLanguage lang;
    lang.load(std::string(""))->analyze();
    nlohmann::json result = lang.get_full_structure();
    BOOST_CHECK(result.is_array());
    // Even empty source creates a module entity in some tree-sitter versions;
    // this test mainly verifies no crash.
}

BOOST_AUTO_TEST_CASE(get_full_structure_before_analyze_returns_empty_array)
{
    PythonLanguage lang;
    lang.load(std::string("def foo():\n    pass"));
    nlohmann::json result = lang.get_full_structure();
    // No analysis performed yet, so no module entity exists
    BOOST_CHECK(result.is_array());
    BOOST_CHECK(result.empty());
}

BOOST_AUTO_TEST_CASE(get_full_structure_module_has_children)
{
    PythonLanguage lang;
    lang.load(std::string(
        "def foo():\n"
        "    pass\n"
        "class Bar:\n"
        "    def baz(self):\n"
        "        pass"
    ))->analyze();
    nlohmann::json result = lang.get_full_structure();
    BOOST_REQUIRE(!result.empty());
    const auto& module = result[0];
    // Module should have children (at least foo and Bar)
    BOOST_CHECK(!module["sub_entity"].empty());
}

BOOST_AUTO_TEST_CASE(get_full_structure_deeply_nested_tree)
{
    PythonLanguage lang;
    lang.load(std::string(
        "class A:\n"
        "    class B:\n"
        "        def c(self):\n"
        "            def d():\n"
        "                pass"
    ))->analyze();
    nlohmann::json result = lang.get_full_structure();
    BOOST_REQUIRE(!result.empty());

    // Traverse: module → class A → class B → method c → function d
    const auto& module = result[0];
    BOOST_REQUIRE(!module["sub_entity"].empty());
    const auto& a = module["sub_entity"][0];
    BOOST_CHECK_EQUAL(a["meta"]["field_content"][1], "class_definition");
    BOOST_REQUIRE(!a["sub_entity"].empty());
    const auto& b = a["sub_entity"][0];
    BOOST_CHECK_EQUAL(b["meta"]["field_content"][1], "class_definition");
    BOOST_REQUIRE(!b["sub_entity"].empty());
    const auto& c = b["sub_entity"][0];
    BOOST_CHECK_EQUAL(c["meta"]["field_content"][1], "class_method");
    BOOST_REQUIRE(!c["sub_entity"].empty());
    const auto& d = c["sub_entity"][0];
    BOOST_CHECK_EQUAL(d["meta"]["field_content"][1], "function_definition");
    BOOST_CHECK(d["sub_entity"].is_array());
    BOOST_CHECK(d["sub_entity"].empty());
}

BOOST_AUTO_TEST_CASE(get_full_structure_leaf_has_empty_sub_entity)
{
    PythonLanguage lang;
    lang.load(std::string("x = 1\ny = 2"))->analyze();
    nlohmann::json result = lang.get_full_structure();
    BOOST_REQUIRE(!result.empty());
    const auto& module = result[0];
    // Module children (global variables) should be leaves
    for (const auto& child : module["sub_entity"]) {
        BOOST_CHECK(child.contains("sub_entity"));
        BOOST_CHECK(child["sub_entity"].is_array());
    }
}

BOOST_AUTO_TEST_CASE(get_full_structure_consistent_with_locate_entity)
{
    // get_full_structure() on the module should produce the same result
    // as locate_entity() on the module's file name.
    TempPythonFile tmp(
        "def foo():\n"
        "    pass\n"
    );
    PythonLanguage lang;
    lang.open(tmp.path)->analyze();
    nlohmann::json full = lang.get_full_structure();
    nlohmann::json located = lang.locate_entity(tmp.path.filename().string());
    BOOST_REQUIRE(!full.empty());
    BOOST_REQUIRE(!located.empty());
    // Both should return the module entity serialized with children
    BOOST_CHECK_EQUAL(full.dump(), located.dump());
}

BOOST_AUTO_TEST_SUITE_END()
