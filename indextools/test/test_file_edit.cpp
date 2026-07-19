#define BOOST_TEST_MODULE FileEditTests
#include <boost/test/unit_test.hpp>

#include "indextools/service_command/file_edit.hpp"
#include "indextools/schema.hpp"

#include <boost/asio.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

using namespace indextools;

namespace {

// ============================================================================
// Coroutine runner — pump a coroutine returning awaitable<nlohmann::json>
// on a fresh io_context and return the result synchronously.
// ============================================================================

template <typename Invocable>
nlohmann::json run(Invocable&& invocable) {
    boost::asio::io_context ctx;
    nlohmann::json result;
    std::exception_ptr eptr;

    boost::asio::co_spawn(
        ctx,
        invocable(),
        [&](std::exception_ptr e, nlohmann::json r) {
            eptr   = e;
            result = std::move(r);
        });

    ctx.run();

    if (eptr) {
        std::rethrow_exception(eptr);
    }
    return result;
}

// ============================================================================
// Helper: create a temporary file with given content, return its path.
// ============================================================================

std::filesystem::path create_temp_file(const std::string& content,
                                        const std::string& suffix = ".txt") {
    // Use a simple deterministic approach: write into a temp directory with
    // a random component so concurrent test runs don't collide.
    auto tmp_dir = std::filesystem::temp_directory_path();
    auto path = tmp_dir / ("cpptools_test_" + random_suffix() + suffix);
    std::ofstream out(path);
    out << content;
    out.close();
    return path;
}

// ============================================================================
// Helper: read file content into a string.
// ============================================================================

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

// ============================================================================
// Helper: extract error code from the first error in a (possibly array) response.
// ============================================================================

std::string error_code_from(const nlohmann::json& report) {
    const auto& r = report.is_array() ? report[0] : report;
    return r.at("meta").at("field_content").at(2).get<std::string>();
}

std::string error_phase_from(const nlohmann::json& report) {
    const auto& r = report.is_array() ? report[0] : report;
    return r.at("meta").at("field_content").at(1).get<std::string>();
}

std::string command_from(const nlohmann::json& report) {
    const auto& r = report.is_array() ? report[0] : report;
    return r.at("meta").at("field_content").at(0).get<std::string>();
}

} // anonymous namespace

// ============================================================================
// Suite: RandomSuffix
// ============================================================================

BOOST_AUTO_TEST_SUITE(RandomSuffixSuite)

BOOST_AUTO_TEST_CASE(returns_six_characters) {
    auto s = random_suffix();
    BOOST_CHECK_EQUAL(s.size(), 6u);
}

BOOST_AUTO_TEST_CASE(all_characters_are_alphanumeric) {
    for (int i = 0; i < 20; ++i) {
        auto s = random_suffix();
        for (char c : s) {
            BOOST_CHECK_MESSAGE(std::isalnum(static_cast<unsigned char>(c)),
                              "character '" << c << "' is not alphanumeric");
        }
    }
}

BOOST_AUTO_TEST_CASE(successive_calls_return_different_values) {
    std::set<std::string> seen;
    for (int i = 0; i < 10; ++i) {
        seen.insert(random_suffix());
    }
    // The probability of 10 consecutive collisions with 62^6 combinations is
    // effectively zero; more than one unique value proves rng state advances.
    BOOST_CHECK_GT(seen.size(), 1u);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: EnsureParams
// ============================================================================

BOOST_AUTO_TEST_SUITE(EnsureParamsSuite)

BOOST_AUTO_TEST_CASE(all_required_fields_produces_normalised_params) {
    LineReplaceEditCommand cmd(3);
    nlohmann::json params;
    params["file_path"]  = "/tmp/test.txt";
    params["line_start"] = 0;
    params["line_end"]   = 5;
    params["content"]    = "new content";

    nlohmann::json error;
    auto ensured = cmd.ensure_params(params, error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK(ensured.is_object());
    BOOST_CHECK_EQUAL(ensured["file_path"].get<std::string>(), "/tmp/test.txt");
    BOOST_CHECK_EQUAL(ensured["line_start"].get<int64_t>(), 0);
    BOOST_CHECK_EQUAL(ensured["line_end"].get<int64_t>(), 5);
    BOOST_CHECK_EQUAL(ensured["content"].get<std::string>(), "new content");
    // Defaults: context_lines from constructor, insert_mode = false.
    BOOST_CHECK_EQUAL(ensured["context_lines"].get<int64_t>(), 3);
    BOOST_CHECK_EQUAL(ensured["insert_mode"].get<bool>(), false);
}

BOOST_AUTO_TEST_CASE(missing_required_field_file_path_returns_error) {
    LineReplaceEditCommand cmd;
    nlohmann::json params;
    params["line_start"] = 0;
    params["line_end"]   = 5;
    params["content"]    = "x";

    nlohmann::json error;
    auto ensured = cmd.ensure_params(params, error);

    BOOST_CHECK(!error.empty());
    BOOST_CHECK(ensured.is_null());  // empty on failure
    BOOST_CHECK_EQUAL(error_code_from(error),
                      std::string(schema::error_code::invalid_argument));
}

BOOST_AUTO_TEST_CASE(missing_required_field_line_start_returns_error) {
    LineReplaceEditCommand cmd;
    nlohmann::json params;
    params["file_path"]  = "/tmp/f.txt";
    params["line_end"]   = 5;
    params["content"]    = "x";

    nlohmann::json error;
    cmd.ensure_params(params, error);

    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(missing_required_field_content_returns_error) {
    LineReplaceEditCommand cmd;
    nlohmann::json params;
    params["file_path"]  = "/tmp/f.txt";
    params["line_start"] = 0;
    params["line_end"]   = 5;

    nlohmann::json error;
    cmd.ensure_params(params, error);

    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(wrong_type_for_line_start_returns_error) {
    LineReplaceEditCommand cmd;
    nlohmann::json params;
    params["file_path"]  = "/tmp/f.txt";
    params["line_start"] = "not_a_number";
    params["line_end"]   = 5;
    params["content"]    = "x";

    nlohmann::json error;
    cmd.ensure_params(params, error);

    BOOST_CHECK(!error.empty());
    BOOST_CHECK_EQUAL(error_code_from(error),
                      std::string(schema::error_code::invalid_argument));
}

BOOST_AUTO_TEST_CASE(negative_line_start_returns_error) {
    LineReplaceEditCommand cmd;
    nlohmann::json params;
    params["file_path"]  = "/tmp/f.txt";
    params["line_start"] = -1;
    params["line_end"]   = 5;
    params["content"]    = "x";

    nlohmann::json error;
    cmd.ensure_params(params, error);

    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(line_start_greater_than_line_end_returns_error) {
    LineReplaceEditCommand cmd;
    nlohmann::json params;
    params["file_path"]  = "/tmp/f.txt";
    params["line_start"] = 5;
    params["line_end"]   = 2;
    params["content"]    = "x";

    nlohmann::json error;
    cmd.ensure_params(params, error);

    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(insert_mode_defaults_to_false) {
    LineReplaceEditCommand cmd;
    nlohmann::json params;
    params["file_path"]  = "/tmp/f.txt";
    params["line_start"] = 0;
    params["line_end"]   = 0;
    params["content"]    = "x";
    // insert_mode not set.

    nlohmann::json error;
    auto ensured = cmd.ensure_params(params, error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(ensured["insert_mode"].get<bool>(), false);
}

BOOST_AUTO_TEST_CASE(insert_mode_explicitly_set_to_true) {
    LineReplaceEditCommand cmd;
    nlohmann::json params;
    params["file_path"]  = "/tmp/f.txt";
    params["line_start"] = 0;
    params["line_end"]   = 0;
    params["content"]    = "x";
    params["insert_mode"] = true;

    nlohmann::json error;
    auto ensured = cmd.ensure_params(params, error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(ensured["insert_mode"].get<bool>(), true);
}

BOOST_AUTO_TEST_CASE(context_lines_defaults_to_constructor_value) {
    LineReplaceEditCommand cmd(7);
    nlohmann::json params;
    params["file_path"]  = "/tmp/f.txt";
    params["line_start"] = 0;
    params["line_end"]   = 0;
    params["content"]    = "x";

    nlohmann::json error;
    auto ensured = cmd.ensure_params(params, error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(ensured["context_lines"].get<int64_t>(), 7);
}

BOOST_AUTO_TEST_CASE(context_lines_override_from_params) {
    LineReplaceEditCommand cmd(3);
    nlohmann::json params;
    params["file_path"]     = "/tmp/f.txt";
    params["line_start"]    = 0;
    params["line_end"]      = 0;
    params["content"]       = "x";
    params["context_lines"] = 10;

    nlohmann::json error;
    auto ensured = cmd.ensure_params(params, error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(ensured["context_lines"].get<int64_t>(), 10);
}

BOOST_AUTO_TEST_CASE(negative_context_lines_returns_error) {
    LineReplaceEditCommand cmd;
    nlohmann::json params;
    params["file_path"]     = "/tmp/f.txt";
    params["line_start"]    = 0;
    params["line_end"]      = 0;
    params["content"]       = "x";
    params["context_lines"] = -1;  // must be >= 0

    nlohmann::json error;
    cmd.ensure_params(params, error);

    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: Execute
// ============================================================================

BOOST_AUTO_TEST_SUITE(ExecuteSuite)

// ---- helper to invoke execute() via the coroutine runner ----

nlohmann::json run_execute(LineReplaceEditCommand& cmd,
                            nlohmann::json ensured_params,
                            nlohmann::json& error_out) {
    return run([&]() -> boost::asio::awaitable<nlohmann::json> {
        co_return co_await cmd.execute(std::move(ensured_params), error_out);
    });
}

BOOST_AUTO_TEST_CASE(file_not_found_returns_not_found_error) {
    LineReplaceEditCommand cmd;
    nlohmann::json ensured;
    ensured["file_path"]     = "/nonexistent/path/test_file_xyz.txt";
    ensured["line_start"]    = static_cast<size_t>(0);
    ensured["line_end"]      = static_cast<size_t>(0);
    ensured["content"]       = "new";
    ensured["context_lines"] = static_cast<size_t>(1);
    ensured["insert_mode"]   = false;

    nlohmann::json error;
    auto result = run_execute(cmd, ensured, error);

    BOOST_CHECK(!error.empty());
    BOOST_CHECK(result.is_null());
    BOOST_CHECK_EQUAL(error_code_from(error),
                      std::string(schema::error_code::not_found));
}

BOOST_AUTO_TEST_CASE(replace_single_line_writes_file_and_returns_diff) {
    LineReplaceEditCommand cmd(0);
    auto path = create_temp_file("line0\nline1\nline2\n");

    // Clean up the temp file at scope exit.
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::filesystem::remove(p); }
    } cleanup{path};

    nlohmann::json ensured;
    ensured["file_path"]     = path.string();
    ensured["line_start"]    = static_cast<size_t>(1);
    ensured["line_end"]      = static_cast<size_t>(1);
    ensured["content"]       = "REPLACED";
    ensured["context_lines"] = static_cast<size_t>(1);
    ensured["insert_mode"]   = false;

    nlohmann::json error;
    auto diff = run_execute(cmd, ensured, error);

    // Should succeed.
    BOOST_CHECK(error.empty());
    BOOST_CHECK(diff.is_array());
    BOOST_CHECK_EQUAL(diff.size(), 2u);

    // File should contain the edit.
    std::string file_content = read_file(path);
    BOOST_CHECK_EQUAL(file_content, "line0\nREPLACED\nline2\n");
}

BOOST_AUTO_TEST_CASE(replace_line_range_writes_correctly) {
    LineReplaceEditCommand cmd(0);
    auto path = create_temp_file("a\nb\nc\nd\ne\n");

    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::filesystem::remove(p); }
    } cleanup{path};

    nlohmann::json ensured;
    ensured["file_path"]     = path.string();
    ensured["line_start"]    = static_cast<size_t>(1);
    ensured["line_end"]      = static_cast<size_t>(3);   // replace b, c, d
    ensured["content"]       = "X\nY";
    ensured["context_lines"] = static_cast<size_t>(0);
    ensured["insert_mode"]   = false;

    nlohmann::json error;
    auto diff = run_execute(cmd, ensured, error);

    BOOST_CHECK(error.empty());
    std::string file_content = read_file(path);
    BOOST_CHECK_EQUAL(file_content, "a\nX\nY\ne\n");
}

BOOST_AUTO_TEST_CASE(insert_mode_inserts_content_between_lines) {
    LineReplaceEditCommand cmd(0);
    auto path = create_temp_file("first\nsecond\n");

    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::filesystem::remove(p); }
    } cleanup{path};

    nlohmann::json ensured;
    ensured["file_path"]     = path.string();
    ensured["line_start"]    = static_cast<size_t>(0);
    ensured["line_end"]      = static_cast<size_t>(0); // ignored in insert mode
    ensured["content"]       = "INSERTED";
    ensured["context_lines"] = static_cast<size_t>(0);
    ensured["insert_mode"]   = true;

    nlohmann::json error;
    auto diff = run_execute(cmd, ensured, error);

    BOOST_CHECK(error.empty());
    std::string file_content = read_file(path);
    BOOST_CHECK_EQUAL(file_content, "first\nINSERTED\nsecond\n");
}

BOOST_AUTO_TEST_CASE(diff_structure_has_expected_blocks) {
    LineReplaceEditCommand cmd(1);
    auto path = create_temp_file("keep\nold_line\nkeep2\n");

    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::filesystem::remove(p); }
    } cleanup{path};

    nlohmann::json ensured;
    ensured["file_path"]     = path.string();
    ensured["line_start"]    = static_cast<size_t>(1);
    ensured["line_end"]      = static_cast<size_t>(1);
    ensured["content"]       = "new_line";
    ensured["context_lines"] = static_cast<size_t>(1);
    ensured["insert_mode"]   = false;

    nlohmann::json error;
    auto diff = run_execute(cmd, ensured, error);

    BOOST_CHECK(error.empty());
    BOOST_REQUIRE(diff.is_array());
    BOOST_REQUIRE_EQUAL(diff.size(), 2u);

    // Block 0: deletions (old_line removed).
    const auto& deletions = diff[0];
    BOOST_CHECK(deletions.contains("meta"));
    BOOST_CHECK(deletions.contains("text"));
    BOOST_CHECK_EQUAL(deletions["meta"]["field_name"][1], "Lines deleted");

    // Block 1: additions (new_line added).
    const auto& additions = diff[1];
    BOOST_CHECK(additions.contains("meta"));
    BOOST_CHECK(additions.contains("text"));
    BOOST_CHECK_EQUAL(additions["meta"]["field_name"][1], "Lines added");
}

BOOST_AUTO_TEST_CASE(replace_with_multiline_content_preserves_line_endings) {
    LineReplaceEditCommand cmd(0);
    // Content with CRLF endings — should be normalised to LF by line_replace_edit.
    auto path = create_temp_file("l0\nl1\nl2\n");

    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::filesystem::remove(p); }
    } cleanup{path};

    nlohmann::json ensured;
    ensured["file_path"]     = path.string();
    ensured["line_start"]    = static_cast<size_t>(1);
    ensured["line_end"]      = static_cast<size_t>(1);
    ensured["content"]       = "A\r\nB\r\nC";
    ensured["context_lines"] = static_cast<size_t>(0);
    ensured["insert_mode"]   = false;

    nlohmann::json error;
    run_execute(cmd, ensured, error);

    BOOST_CHECK(error.empty());
    std::string file_content = read_file(path);
    // CRLF should be normalised to LF.
    BOOST_CHECK_EQUAL(file_content, "l0\nA\nB\nC\nl2\n");
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: ServiceCommandIntegration
// ============================================================================

BOOST_AUTO_TEST_SUITE(ServiceCommandIntegrationSuite)

BOOST_AUTO_TEST_CASE(operator_parentheses_runs_full_pipeline) {
    LineReplaceEditCommand cmd(0);
    // "hello\nworld\n" splits into 3 chunks: ["hello", "world", ""].
    // Replacing line 1 ("world") with "earth" → "hello\nearth\n".
    auto path = create_temp_file("hello\nworld\n");

    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::filesystem::remove(p); }
    } cleanup{path};

    nlohmann::json params;
    params["file_path"]     = path.string();
    params["line_start"]    = 1;
    params["line_end"]      = 1;
    params["content"]       = "earth";
    params["context_lines"] = 0;

    nlohmann::json result = run([&]() { return cmd(std::move(params)); });

    // Should return a diff array (two blocks), not an error.
    BOOST_CHECK(result.is_array());
    BOOST_CHECK_EQUAL(result.size(), 2u);

    // File should be updated: "world" → "earth".
    BOOST_CHECK_EQUAL(read_file(path), "hello\nearth\n");
}

BOOST_AUTO_TEST_CASE(operator_returns_validation_error_for_missing_field) {
    LineReplaceEditCommand cmd;
    nlohmann::json params;
    params["line_start"] = 0;
    params["line_end"]   = 0;
    params["content"]    = "x";
    // file_path missing.

    nlohmann::json result = run([&]() { return cmd(std::move(params)); });

    // Should be a structured error report array.
    BOOST_REQUIRE(result.is_array());
    BOOST_REQUIRE(!result.empty());
    const auto& err = result[0];
    BOOST_CHECK(err.contains("meta"));
    BOOST_CHECK(err.contains("message"));
    BOOST_CHECK_EQUAL(command_from(result), "file_edit.line_replace_edit");
    BOOST_CHECK_EQUAL(error_phase_from(result),
                      std::string(schema::error_phase::validation));
    BOOST_CHECK_EQUAL(error_code_from(result),
                      std::string(schema::error_code::invalid_argument));
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: StrEnsureParams
// ============================================================================

BOOST_AUTO_TEST_SUITE(StrEnsureParamsSuite)

BOOST_AUTO_TEST_CASE(all_required_fields_produces_normalised_params) {
    StrReplaceEditCommand cmd(3);
    nlohmann::json params;
    params["file_path"]        = "/tmp/test.txt";
    params["original_content"] = "old text";
    params["inserted_content"] = "new text";

    nlohmann::json error;
    auto ensured = cmd.ensure_params(params, error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK(ensured.is_object());
    BOOST_CHECK_EQUAL(ensured["file_path"].get<std::string>(), "/tmp/test.txt");
    BOOST_CHECK_EQUAL(ensured["original_content"].get<std::string>(), "old text");
    BOOST_CHECK_EQUAL(ensured["inserted_content"].get<std::string>(), "new text");
    // Defaults: context_lines from constructor, replace_all = false.
    BOOST_CHECK_EQUAL(ensured["context_lines"].get<int64_t>(), 3);
    BOOST_CHECK_EQUAL(ensured["replace_all"].get<bool>(), false);
}

BOOST_AUTO_TEST_CASE(missing_required_field_file_path_returns_error) {
    StrReplaceEditCommand cmd;
    nlohmann::json params;
    params["original_content"] = "old";
    params["inserted_content"] = "new";

    nlohmann::json error;
    auto ensured = cmd.ensure_params(params, error);

    BOOST_CHECK(!error.empty());
    BOOST_CHECK(ensured.is_null());
    BOOST_CHECK_EQUAL(error_code_from(error),
                      std::string(schema::error_code::invalid_argument));
}

BOOST_AUTO_TEST_CASE(missing_required_field_original_content_returns_error) {
    StrReplaceEditCommand cmd;
    nlohmann::json params;
    params["file_path"]        = "/tmp/f.txt";
    params["inserted_content"] = "new";

    nlohmann::json error;
    cmd.ensure_params(params, error);

    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(missing_required_field_inserted_content_returns_error) {
    StrReplaceEditCommand cmd;
    nlohmann::json params;
    params["file_path"]        = "/tmp/f.txt";
    params["original_content"] = "old";

    nlohmann::json error;
    cmd.ensure_params(params, error);

    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(wrong_type_for_original_content_returns_error) {
    StrReplaceEditCommand cmd;
    nlohmann::json params;
    params["file_path"]        = "/tmp/f.txt";
    params["original_content"] = 42;   // should be string
    params["inserted_content"] = "new";

    nlohmann::json error;
    cmd.ensure_params(params, error);

    BOOST_CHECK(!error.empty());
    BOOST_CHECK_EQUAL(error_code_from(error),
                      std::string(schema::error_code::invalid_argument));
}

BOOST_AUTO_TEST_CASE(replace_all_defaults_to_false) {
    StrReplaceEditCommand cmd;
    nlohmann::json params;
    params["file_path"]        = "/tmp/f.txt";
    params["original_content"] = "old";
    params["inserted_content"] = "new";
    // replace_all not set.

    nlohmann::json error;
    auto ensured = cmd.ensure_params(params, error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(ensured["replace_all"].get<bool>(), false);
}

BOOST_AUTO_TEST_CASE(replace_all_explicitly_set_to_true) {
    StrReplaceEditCommand cmd;
    nlohmann::json params;
    params["file_path"]        = "/tmp/f.txt";
    params["original_content"] = "old";
    params["inserted_content"] = "new";
    params["replace_all"]      = true;

    nlohmann::json error;
    auto ensured = cmd.ensure_params(params, error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(ensured["replace_all"].get<bool>(), true);
}

BOOST_AUTO_TEST_CASE(context_lines_defaults_to_constructor_value) {
    StrReplaceEditCommand cmd(7);
    nlohmann::json params;
    params["file_path"]        = "/tmp/f.txt";
    params["original_content"] = "old";
    params["inserted_content"] = "new";

    nlohmann::json error;
    auto ensured = cmd.ensure_params(params, error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(ensured["context_lines"].get<int64_t>(), 7);
}

BOOST_AUTO_TEST_CASE(context_lines_override_from_params) {
    StrReplaceEditCommand cmd(3);
    nlohmann::json params;
    params["file_path"]        = "/tmp/f.txt";
    params["original_content"] = "old";
    params["inserted_content"] = "new";
    params["context_lines"]    = 10;

    nlohmann::json error;
    auto ensured = cmd.ensure_params(params, error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(ensured["context_lines"].get<int64_t>(), 10);
}

BOOST_AUTO_TEST_CASE(negative_context_lines_returns_error) {
    StrReplaceEditCommand cmd;
    nlohmann::json params;
    params["file_path"]        = "/tmp/f.txt";
    params["original_content"] = "old";
    params["inserted_content"] = "new";
    params["context_lines"]    = -1;  // must be >= 0

    nlohmann::json error;
    cmd.ensure_params(params, error);

    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: StrExecute
// ============================================================================

BOOST_AUTO_TEST_SUITE(StrExecuteSuite)

// ---- helper to invoke execute() via the coroutine runner ----

nlohmann::json run_str_execute(StrReplaceEditCommand& cmd,
                                nlohmann::json ensured_params,
                                nlohmann::json& error_out) {
    return run([&]() -> boost::asio::awaitable<nlohmann::json> {
        co_return co_await cmd.execute(std::move(ensured_params), error_out);
    });
}

BOOST_AUTO_TEST_CASE(file_not_found_returns_not_found_error) {
    StrReplaceEditCommand cmd;
    nlohmann::json ensured;
    ensured["file_path"]        = "/nonexistent/path/test_file_str_xyz.txt";
    ensured["original_content"] = "old";
    ensured["inserted_content"] = "new";
    ensured["context_lines"]    = static_cast<size_t>(0);
    ensured["replace_all"]      = false;

    nlohmann::json error;
    auto result = run_str_execute(cmd, ensured, error);

    BOOST_CHECK(!error.empty());
    BOOST_CHECK(result.is_null());
    BOOST_CHECK_EQUAL(error_code_from(error),
                      std::string(schema::error_code::not_found));
}

BOOST_AUTO_TEST_CASE(single_replacement_writes_file_and_returns_diff) {
    StrReplaceEditCommand cmd(0);
    auto path = create_temp_file("hello world\nfoo bar\nbaz qux\n");

    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::filesystem::remove(p); }
    } cleanup{path};

    nlohmann::json ensured;
    ensured["file_path"]        = path.string();
    ensured["original_content"] = "foo bar";
    ensured["inserted_content"] = "REPLACED";
    ensured["context_lines"]    = static_cast<size_t>(0);
    ensured["replace_all"]      = false;

    nlohmann::json error;
    auto diff = run_str_execute(cmd, ensured, error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK(diff.is_array());
    std::string file_content = read_file(path);
    BOOST_CHECK_EQUAL(file_content, "hello world\nREPLACED\nbaz qux\n");
}

BOOST_AUTO_TEST_CASE(replace_all_occurrences) {
    StrReplaceEditCommand cmd(0);
    auto path = create_temp_file("apple banana apple cherry apple\n");

    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::filesystem::remove(p); }
    } cleanup{path};

    nlohmann::json ensured;
    ensured["file_path"]        = path.string();
    ensured["original_content"] = "apple";
    ensured["inserted_content"] = "ORANGE";
    ensured["context_lines"]    = static_cast<size_t>(0);
    ensured["replace_all"]      = true;

    nlohmann::json error;
    auto diff = run_str_execute(cmd, ensured, error);

    BOOST_CHECK(error.empty());
    std::string file_content = read_file(path);
    BOOST_CHECK_EQUAL(file_content, "ORANGE banana ORANGE cherry ORANGE\n");
}

BOOST_AUTO_TEST_CASE(pattern_not_found_returns_error) {
    StrReplaceEditCommand cmd;
    auto path = create_temp_file("line1\nline2\n");

    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::filesystem::remove(p); }
    } cleanup{path};

    nlohmann::json ensured;
    ensured["file_path"]        = path.string();
    ensured["original_content"] = "nonexistent_pattern";
    ensured["inserted_content"] = "replacement";
    ensured["context_lines"]    = static_cast<size_t>(0);
    ensured["replace_all"]      = false;

    nlohmann::json error;
    auto result = run_str_execute(cmd, ensured, error);

    BOOST_CHECK(!error.empty());
    BOOST_CHECK(result.is_null());
    BOOST_CHECK_EQUAL(error_code_from(error),
                      std::string(schema::error_code::internal_error));
}

BOOST_AUTO_TEST_CASE(multiple_matches_without_replace_all_returns_error) {
    StrReplaceEditCommand cmd;
    auto path = create_temp_file("dup dup dup\n");

    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::filesystem::remove(p); }
    } cleanup{path};

    nlohmann::json ensured;
    ensured["file_path"]        = path.string();
    ensured["original_content"] = "dup";
    ensured["inserted_content"] = "unique";
    ensured["context_lines"]    = static_cast<size_t>(0);
    ensured["replace_all"]      = false;  // 3 matches but only 1 expected

    nlohmann::json error;
    auto result = run_str_execute(cmd, ensured, error);

    BOOST_CHECK(!error.empty());
    BOOST_CHECK(result.is_null());
}

BOOST_AUTO_TEST_CASE(diff_structure_has_expected_blocks) {
    StrReplaceEditCommand cmd(1);
    auto path = create_temp_file("keep this\nold_pattern\nkeep this too\n");

    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::filesystem::remove(p); }
    } cleanup{path};

    nlohmann::json ensured;
    ensured["file_path"]        = path.string();
    ensured["original_content"] = "old_pattern";
    ensured["inserted_content"] = "new_pattern";
    ensured["context_lines"]    = static_cast<size_t>(1);
    ensured["replace_all"]      = false;

    nlohmann::json error;
    auto diff = run_str_execute(cmd, ensured, error);

    BOOST_CHECK(error.empty());
    BOOST_REQUIRE(diff.is_array());
    BOOST_REQUIRE_EQUAL(diff.size(), 2u);

    // Block 0: deletions (old_pattern removed).
    const auto& deletions = diff[0];
    BOOST_CHECK(deletions.contains("meta"));
    BOOST_CHECK(deletions.contains("text"));
    BOOST_CHECK_EQUAL(deletions["meta"]["field_name"][1], "Lines deleted");

    // Block 1: additions (new_pattern added).
    const auto& additions = diff[1];
    BOOST_CHECK(additions.contains("meta"));
    BOOST_CHECK(additions.contains("text"));
    BOOST_CHECK_EQUAL(additions["meta"]["field_name"][1], "Lines added");
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: StrServiceCommandIntegration
// ============================================================================

BOOST_AUTO_TEST_SUITE(StrServiceCommandIntegrationSuite)

BOOST_AUTO_TEST_CASE(operator_parentheses_runs_full_pipeline) {
    StrReplaceEditCommand cmd(0);
    auto path = create_temp_file("the quick brown fox\n");

    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::filesystem::remove(p); }
    } cleanup{path};

    nlohmann::json params;
    params["file_path"]        = path.string();
    params["original_content"] = "brown";
    params["inserted_content"] = "red";
    params["context_lines"]    = 0;

    nlohmann::json result = run([&]() { return cmd(std::move(params)); });

    // Should return a diff array (two blocks), not an error.
    BOOST_CHECK(result.is_array());
    BOOST_CHECK_EQUAL(result.size(), 2u);

    // File should be updated: "brown" → "red".
    BOOST_CHECK_EQUAL(read_file(path), "the quick red fox\n");
}

BOOST_AUTO_TEST_CASE(operator_returns_validation_error_for_missing_field) {
    StrReplaceEditCommand cmd;
    nlohmann::json params;
    params["original_content"] = "old";
    params["inserted_content"] = "new";
    // file_path missing.

    nlohmann::json result = run([&]() { return cmd(std::move(params)); });

    // Should be a structured error report array.
    BOOST_REQUIRE(result.is_array());
    BOOST_REQUIRE(!result.empty());
    const auto& err = result[0];
    BOOST_CHECK(err.contains("meta"));
    BOOST_CHECK(err.contains("message"));
    BOOST_CHECK_EQUAL(command_from(result), "file_edit.str_replace_edit");
    BOOST_CHECK_EQUAL(error_phase_from(result),
                      std::string(schema::error_phase::validation));
    BOOST_CHECK_EQUAL(error_code_from(result),
                      std::string(schema::error_code::invalid_argument));
}

BOOST_AUTO_TEST_SUITE_END()
