#define BOOST_TEST_MODULE ShellCommandTests
#include <boost/test/unit_test.hpp>

#include "service_command/shell.hpp"
#include "service_command/service_command.hpp"
#include "schema.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace indextools;

// ============================================================================
// Suite: parse_duration_to_seconds
// ============================================================================

BOOST_AUTO_TEST_SUITE(ParseDurationToSeconds)

// ---- basic single-unit cases ----

BOOST_AUTO_TEST_CASE(bare_seconds) {
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("0s"), 0);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("1s"), 1);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("30s"), 30);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("90s"), 90);
}

BOOST_AUTO_TEST_CASE(bare_minutes) {
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("0m"), 0);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("1m"), 60);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("5m"), 300);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("10m"), 600);
}

BOOST_AUTO_TEST_CASE(bare_hours) {
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("0h"), 0);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("1h"), 3600);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("2h"), 7200);
}

BOOST_AUTO_TEST_CASE(bare_days) {
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("0d"), 0);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("1d"), 86400);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("3d"), 259200);
}

BOOST_AUTO_TEST_CASE(bare_weeks) {
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("0w"), 0);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("1w"), 604800);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("2w"), 1209600);
}

// ---- case insensitivity ----

BOOST_AUTO_TEST_CASE(uppercase_units) {
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("30S"), 30);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("5M"), 300);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("1H"), 3600);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("1D"), 86400);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("1W"), 604800);
}

BOOST_AUTO_TEST_CASE(mixed_case_units) {
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("1H30m"), 5400);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("2W3d"), 1468800);
}

// ---- implicit "1" when number omitted ----

BOOST_AUTO_TEST_CASE(implicit_value_unit_only) {
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("s"), 1);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("m"), 60);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("h"), 3600);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("d"), 86400);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("w"), 604800);
}

BOOST_AUTO_TEST_CASE(implicit_value_in_compound) {
    // "h30m" = 1 hour + 30 minutes = 5400
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("h30m"), 5400);
    // "30mh" = 30 minutes + 1 hour = 5400
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("30mh"), 5400);
}

// ---- compound durations ----

BOOST_AUTO_TEST_CASE(compound_two_units) {
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("1h30m"), 5400);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("2m30s"), 150);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("1d12h"), 129600);
}

BOOST_AUTO_TEST_CASE(compound_three_units) {
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("1h30m45s"), 5445);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("1d2h3m"), 93780);
}

BOOST_AUTO_TEST_CASE(compound_all_units) {
    // 2w3d4h5m6s = 2*604800 + 3*86400 + 4*3600 + 5*60 + 6*1
    size_t expected = 2*604800 + 3*86400 + 4*3600 + 5*60 + 6*1;
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("2w3d4h5m6s"), expected);
}

// ---- trailing bare number (seconds) ----

BOOST_AUTO_TEST_CASE(trailing_bare_number) {
    // "5m30" = 5*60 + 30 = 330
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("5m30"), 330);
    // "1h15" = 3600 + 15 = 3615
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("1h15"), 3615);
}

BOOST_AUTO_TEST_CASE(bare_number_only) {
    // No unit at all — just a bare number.  This is the trailing-number path
    // firing on the only number present.
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("42"), 42);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("0"), 0);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("3600"), 3600);
}

// ---- zero-edge cases ----

BOOST_AUTO_TEST_CASE(zero_values) {
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("0s"), 0);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("0m"), 0);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("0h"), 0);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("0m0s"), 0);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("0h0m0s"), 0);
    // Leading zeros
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("00s"), 0);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("005m"), 300);
}

// ---- overflow detection ----

BOOST_AUTO_TEST_CASE(overflow_on_multiplication) {
    // A value so large that multiplying by 60 overflows.
    // SIZE_MAX / 60 + 1 will overflow when multiplied by 60.
    std::string huge = std::to_string(SIZE_MAX / 60 + 1) + "m";
    BOOST_CHECK_THROW(parse_duration_to_seconds(huge), std::overflow_error);
}

BOOST_AUTO_TEST_CASE(overflow_on_cumulative_addition) {
    // Two large values that are each individually safe but whose sum overflows.
    std::string v1 = std::to_string(SIZE_MAX - 1) + "s";
    BOOST_CHECK_THROW(parse_duration_to_seconds(v1 + "2s"), std::overflow_error);
}

BOOST_AUTO_TEST_CASE(overflow_on_parse) {
    // A huge digit sequence that overflows even before any unit.
    std::string huge(50, '9');
    BOOST_CHECK_THROW(parse_duration_to_seconds(huge), std::overflow_error);
}

BOOST_AUTO_TEST_CASE(no_overflow_near_limit) {
    // SIZE_MAX seconds — should be fine (bare number, no multiplication).
    std::string max_str = std::to_string(SIZE_MAX);
    BOOST_CHECK_EQUAL(parse_duration_to_seconds(max_str), SIZE_MAX);
}

// ---- invalid inputs ----

BOOST_AUTO_TEST_CASE(empty_string_throws) {
    BOOST_CHECK_THROW(parse_duration_to_seconds(""), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(invalid_unit_throws) {
    BOOST_CHECK_THROW(parse_duration_to_seconds("1x"), std::invalid_argument);
    BOOST_CHECK_THROW(parse_duration_to_seconds("10y"), std::invalid_argument);
    BOOST_CHECK_THROW(parse_duration_to_seconds("5m3x"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(invalid_character_throws) {
    BOOST_CHECK_THROW(parse_duration_to_seconds("5m 30s"), std::invalid_argument);  // space
    BOOST_CHECK_THROW(parse_duration_to_seconds("1.5h"), std::invalid_argument);    // decimal point
    BOOST_CHECK_THROW(parse_duration_to_seconds("5m-30s"), std::invalid_argument);  // dash
    BOOST_CHECK_THROW(parse_duration_to_seconds("@#$"), std::invalid_argument);     // symbols
}

// ---- large but valid values ----

BOOST_AUTO_TEST_CASE(large_but_valid_durations) {
    // 1000 hours = 3,600,000 seconds
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("1000h"), 3600000);
    // 500 days = 43,200,000 seconds
    BOOST_CHECK_EQUAL(parse_duration_to_seconds("500d"), 43200000);
}

BOOST_AUTO_TEST_CASE(max_value_multiplication) {
    // Largest value that won't overflow when multiplied by 60 (minutes).
    size_t max_m = SIZE_MAX / 60;
    std::string input = std::to_string(max_m) + "m";
    BOOST_CHECK_EQUAL(parse_duration_to_seconds(input), max_m * 60);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// ensure_params never dereferences _manager, so a nullptr shared_ptr is safe
// for these tests.  Only execute() touches the manager.
// Declared at file scope so every suite can use it.
static std::shared_ptr<SubProcessManager> null_manager;

// ============================================================================
// Suite: SpawnWaitCommand::ensure_params
// ============================================================================

BOOST_AUTO_TEST_SUITE(SpawnWaitCommandEnsureParams)

// ---- happy path ----

BOOST_AUTO_TEST_CASE(happy_path_minimal_params) {
    SpawnWaitCommand cmd(null_manager);

    nlohmann::json params;
    params["exec_name"] = "echo";
    params["arguments"] = nlohmann::json::array({"hello"});

    nlohmann::json error = nlohmann::json::array();
    nlohmann::json result = cmd.ensure_params(std::move(params), error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK(result.is_object());
    BOOST_CHECK_EQUAL(result.at("exec_name").get<std::string>(), "echo");
    BOOST_CHECK_EQUAL(result.at("timeout_seconds").get<size_t>(), 3); // default
    BOOST_CHECK_EQUAL(result.at("detach_after_timeout").get<bool>(), true); // default
    BOOST_CHECK_EQUAL(result.at("description").get<std::string>(), "echo hello");
    BOOST_CHECK(result.at("arguments").is_array());
    BOOST_CHECK_EQUAL(result.at("arguments").size(), 1);
    BOOST_CHECK_EQUAL(result.at("arguments")[0].get<std::string>(), "hello");
}

BOOST_AUTO_TEST_CASE(happy_path_with_optional_params) {
    SpawnWaitCommand cmd(null_manager);

    nlohmann::json params;
    params["exec_name"] = "gcc";
    params["arguments"] = nlohmann::json::array({"-c", "file.cpp"});
    params["timeout"] = "5m";
    params["detach_after_timeout"] = false;
    params["description"] = "Compile file.cpp";

    nlohmann::json error = nlohmann::json::array();
    nlohmann::json result = cmd.ensure_params(std::move(params), error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(result.at("exec_name").get<std::string>(), "gcc");
    BOOST_CHECK_EQUAL(result.at("timeout_seconds").get<size_t>(), 300); // 5m
    BOOST_CHECK_EQUAL(result.at("detach_after_timeout").get<bool>(), false);
    BOOST_CHECK_EQUAL(result.at("description").get<std::string>(), "Compile file.cpp");
    BOOST_CHECK_EQUAL(result.at("arguments").size(), 2);
}

BOOST_AUTO_TEST_CASE(multiple_arguments) {
    SpawnWaitCommand cmd(null_manager);

    nlohmann::json params;
    params["exec_name"] = "python";
    params["arguments"] = nlohmann::json::array({"-c", "print(1)", "--verbose"});

    nlohmann::json error = nlohmann::json::array();
    nlohmann::json result = cmd.ensure_params(std::move(params), error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(result.at("arguments").size(), 3);
    BOOST_CHECK_EQUAL(result.at("arguments")[2].get<std::string>(), "--verbose");
}

BOOST_AUTO_TEST_CASE(empty_arguments_array) {
    SpawnWaitCommand cmd(null_manager);

    nlohmann::json params;
    params["exec_name"] = "ls";
    params["arguments"] = nlohmann::json::array();

    nlohmann::json error = nlohmann::json::array();
    nlohmann::json result = cmd.ensure_params(std::move(params), error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(result.at("arguments").size(), 0);
    BOOST_CHECK_EQUAL(result.at("description").get<std::string>(), "ls");
}

// ---- auto-generated description ----

BOOST_AUTO_TEST_CASE(auto_description_no_arguments) {
    SpawnWaitCommand cmd(null_manager);

    nlohmann::json params;
    params["exec_name"] = "date";
    params["arguments"] = nlohmann::json::array();

    nlohmann::json error = nlohmann::json::array();
    nlohmann::json result = cmd.ensure_params(std::move(params), error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(result.at("description").get<std::string>(), "date");
}

BOOST_AUTO_TEST_CASE(auto_description_multiple_arguments) {
    SpawnWaitCommand cmd(null_manager);

    nlohmann::json params;
    params["exec_name"] = "git";
    params["arguments"] = nlohmann::json::array({"log", "--oneline", "-n", "10"});

    nlohmann::json error = nlohmann::json::array();
    nlohmann::json result = cmd.ensure_params(std::move(params), error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(result.at("description").get<std::string>(),
                      "git log --oneline -n 10");
}

// ---- missing fields ----

BOOST_AUTO_TEST_CASE(missing_exec_name_is_error) {
    SpawnWaitCommand cmd(null_manager);

    nlohmann::json params;
    params["arguments"] = nlohmann::json::array({"test"});
    // NB: exec_name is missing

    nlohmann::json error = nlohmann::json::array();
    nlohmann::json result = cmd.ensure_params(std::move(params), error);

    BOOST_CHECK(!error.empty());
    BOOST_CHECK(result.empty());
}

BOOST_AUTO_TEST_CASE(missing_arguments_is_error) {
    SpawnWaitCommand cmd(null_manager);

    nlohmann::json params;
    params["exec_name"] = "echo";
    // NB: arguments is missing → params["arguments"] returns null, which
    //     is not an array, so the validation throws.

    nlohmann::json error = nlohmann::json::array();
    nlohmann::json result = cmd.ensure_params(std::move(params), error);

    BOOST_CHECK(!error.empty());
    BOOST_CHECK(result.empty());
}

// ---- invalid argument types ----

BOOST_AUTO_TEST_CASE(arguments_not_an_array) {
    SpawnWaitCommand cmd(null_manager);

    nlohmann::json params;
    params["exec_name"] = "echo";
    params["arguments"] = "not_an_array";

    nlohmann::json error = nlohmann::json::array();
    nlohmann::json result = cmd.ensure_params(std::move(params), error);

    BOOST_CHECK(!error.empty());
    BOOST_CHECK(result.empty());
}

BOOST_AUTO_TEST_CASE(arguments_contains_non_string) {
    SpawnWaitCommand cmd(null_manager);

    nlohmann::json params;
    params["exec_name"] = "echo";
    params["arguments"] = nlohmann::json::array({"valid", 42, "also_valid"});

    nlohmann::json error = nlohmann::json::array();
    nlohmann::json result = cmd.ensure_params(std::move(params), error);

    BOOST_CHECK(!error.empty());
    BOOST_CHECK(result.empty());
}

BOOST_AUTO_TEST_CASE(exec_name_not_a_string) {
    SpawnWaitCommand cmd(null_manager);

    nlohmann::json params;
    params["exec_name"] = 123;  // number, not string
    params["arguments"] = nlohmann::json::array({"test"});

    nlohmann::json error = nlohmann::json::array();
    nlohmann::json result = cmd.ensure_params(std::move(params), error);

    BOOST_CHECK(!error.empty());
    BOOST_CHECK(result.empty());
}

// ---- timeout parsing edge cases ----

BOOST_AUTO_TEST_CASE(timeout_default_is_3_seconds) {
    SpawnWaitCommand cmd(null_manager);

    nlohmann::json params;
    params["exec_name"] = "sleep";
    params["arguments"] = nlohmann::json::array({"1"});
    // timeout not provided → default "3s"

    nlohmann::json error = nlohmann::json::array();
    nlohmann::json result = cmd.ensure_params(std::move(params), error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(result.at("timeout_seconds").get<size_t>(), 3);
}

BOOST_AUTO_TEST_CASE(timeout_custom_value) {
    SpawnWaitCommand cmd(null_manager);

    nlohmann::json params;
    params["exec_name"] = "sleep";
    params["arguments"] = nlohmann::json::array({"60"});
    params["timeout"] = "2m";

    nlohmann::json error = nlohmann::json::array();
    nlohmann::json result = cmd.ensure_params(std::move(params), error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(result.at("timeout_seconds").get<size_t>(), 120);
}

BOOST_AUTO_TEST_CASE(timeout_invalid_format_throws) {
    SpawnWaitCommand cmd(null_manager);

    nlohmann::json params;
    params["exec_name"] = "sleep";
    params["arguments"] = nlohmann::json::array({"1"});
    params["timeout"] = "1x";  // invalid unit

    nlohmann::json error = nlohmann::json::array();
    // parse_duration_to_seconds throws; ensure_params lets the exception
    // propagate (it is not caught inside ensure_params).  The caller
    // (ServiceCommand::operator()) catches it and converts to an error.
    BOOST_CHECK_THROW(cmd.ensure_params(std::move(params), error), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(timeout_empty_string_throws) {
    SpawnWaitCommand cmd(null_manager);

    nlohmann::json params;
    params["exec_name"] = "sleep";
    params["arguments"] = nlohmann::json::array({"1"});
    params["timeout"] = "";

    nlohmann::json error = nlohmann::json::array();
    BOOST_CHECK_THROW(cmd.ensure_params(std::move(params), error), std::invalid_argument);
}

// ---- detach_after_timeout edge cases ----

BOOST_AUTO_TEST_CASE(detach_after_timeout_default_true) {
    SpawnWaitCommand cmd(null_manager);

    nlohmann::json params;
    params["exec_name"] = "test";
    params["arguments"] = nlohmann::json::array();

    nlohmann::json error = nlohmann::json::array();
    nlohmann::json result = cmd.ensure_params(std::move(params), error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(result.at("detach_after_timeout").get<bool>(), true);
}

BOOST_AUTO_TEST_CASE(detach_after_timeout_explicit_false) {
    SpawnWaitCommand cmd(null_manager);

    nlohmann::json params;
    params["exec_name"] = "test";
    params["arguments"] = nlohmann::json::array();
    params["detach_after_timeout"] = false;

    nlohmann::json error = nlohmann::json::array();
    nlohmann::json result = cmd.ensure_params(std::move(params), error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(result.at("detach_after_timeout").get<bool>(), false);
}

// ---- error accumulation: multiple field errors in one call ----

BOOST_AUTO_TEST_CASE(multiple_errors_accumulate) {
    SpawnWaitCommand cmd(null_manager);

    nlohmann::json params;
    params["exec_name"] = 123;               // wrong type
    params["arguments"] = "not_an_array";    // wrong type
    // timeout will also be invalid if the code reaches it, but it won't —
    // parse_duration_to_seconds is only called after the error check.

    nlohmann::json error = nlohmann::json::array();
    nlohmann::json result = cmd.ensure_params(std::move(params), error);

    BOOST_CHECK(!error.empty());
    BOOST_CHECK(result.empty());
    // We should have at least 2 errors (exec_name type + arguments type)
    BOOST_CHECK_GE(error.size(), 2);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: SpawnWaitCommand::name
// ============================================================================

BOOST_AUTO_TEST_SUITE(SpawnWaitCommandName)

BOOST_AUTO_TEST_CASE(name_matches_expected) {
    SpawnWaitCommand cmd(null_manager);
    BOOST_CHECK_EQUAL(cmd.name(), "shell.spawn_and_wait");
}

BOOST_AUTO_TEST_SUITE_END()
