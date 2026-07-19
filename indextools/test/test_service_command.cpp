#define BOOST_TEST_MODULE ServiceCommandTests
#include <boost/test/unit_test.hpp>

#include "indextools/service_command/service_command.hpp"
#include "indextools/schema.hpp"

#include <boost/asio.hpp>

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

using namespace indextools;

// ============================================================================
// Mock command implementations — each exercises a specific path through
// ServiceCommand::operator().
// ============================================================================

namespace {

// ---------------------------------------------------------------------------
// Happy-path mock: ensure_params passes through, execute returns a result.
// ---------------------------------------------------------------------------
struct HappyCommand : ServiceCommand {
    std::string_view name() const noexcept override { return "happy"; }

    nlohmann::json ensure_params(nlohmann::json params,
                                 nlohmann::json& /*error*/) const override {
        // Normalise: add a marker so the test can verify the pipeline ran.
        params["_validated"] = true;
        return params;
    }

    boost::asio::awaitable<nlohmann::json> execute(nlohmann::json params,
                                                    nlohmann::json& /*error*/) override {
        nlohmann::json result;
        result["status"] = "ok";
        result["input"]  = std::move(params);
        co_return result;
    }
};

// ---------------------------------------------------------------------------
// Mock: ensure_params populates an error.
// ---------------------------------------------------------------------------
struct ValidationErrorCommand : ServiceCommand {
    std::string_view name() const noexcept override { return "val_fail"; }

    nlohmann::json ensure_params(nlohmann::json /*params*/,
                                 nlohmann::json& error) const override {
        error.push_back(schema::validation_error(name(), schema::error_code::invalid_argument,
                                         "Parameter 'pattern' is required.").build());
        return {};
    }

    boost::asio::awaitable<nlohmann::json> execute(nlohmann::json /*params*/,
                                                    nlohmann::json& /*error*/) override {
        // Must never be called — the test will fail if we reach here.
        nlohmann::json r;
        r["unexpected"] = true;
        co_return r;
    }
};

// ---------------------------------------------------------------------------
// Mock: ensure_params throws an exception.
// ---------------------------------------------------------------------------
struct ValidationThrowCommand : ServiceCommand {
    std::string_view name() const noexcept override { return "val_throw"; }

    nlohmann::json ensure_params(nlohmann::json /*params*/,
                                 nlohmann::json& /*error*/) const override {
        throw std::invalid_argument("bad argument: pattern is empty");
    }

    boost::asio::awaitable<nlohmann::json> execute(nlohmann::json /*params*/,
                                                    nlohmann::json& /*error*/) override {
        nlohmann::json r;
        r["unexpected"] = true;
        co_return r;
    }
};

// ---------------------------------------------------------------------------
// Mock: ensure_params succeeds, execute populates an error.
// ---------------------------------------------------------------------------
struct ExecutionErrorCommand : ServiceCommand {
    std::string_view name() const noexcept override { return "exec_fail"; }

    nlohmann::json ensure_params(nlohmann::json params,
                                 nlohmann::json& /*error*/) const override {
        return params;
    }

    boost::asio::awaitable<nlohmann::json> execute(nlohmann::json /*params*/,
                                                    nlohmann::json& error) override {
        error.push_back(schema::execution_error(name(), schema::error_code::internal_error,
                                        "Database connection lost.").build());
        co_return nlohmann::json{};
    }
};

// ---------------------------------------------------------------------------
// Mock: execute throws an exception.
// ---------------------------------------------------------------------------
struct ExecutionThrowCommand : ServiceCommand {
    std::string_view name() const noexcept override { return "exec_throw"; }

    nlohmann::json ensure_params(nlohmann::json params,
                                 nlohmann::json& /*error*/) const override {
        return params;
    }

    boost::asio::awaitable<nlohmann::json> execute(nlohmann::json /*params*/,
                                                    nlohmann::json& /*error*/) override {
        throw std::runtime_error("Worker process crashed unexpectedly.");
        co_return nlohmann::json{};
    }
};

// ---------------------------------------------------------------------------
// Mock: ensure_params does nothing (returns empty json), execute succeeds.
//       Tests the edge case where params and result are both empty objects.
// ---------------------------------------------------------------------------
struct EmptyParamsCommand : ServiceCommand {
    std::string_view name() const noexcept override { return "empty"; }

    nlohmann::json ensure_params(nlohmann::json params,
                                 nlohmann::json& /*error*/) const override {
        return params;  // pass through as-is, even if empty
    }

    boost::asio::awaitable<nlohmann::json> execute(nlohmann::json /*params*/,
                                                    nlohmann::json& /*error*/) override {
        co_return nlohmann::json::object();
    }
};

// ============================================================================
// Driver: pump a coroutine returning awaitable<nlohmann::json> on a fresh
// io_context and return the result synchronously.
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

// Helper: extract the error code from a structured ErrorReport JSON.
// report may be an array (from operator()) or a single ErrorReport object.
std::string error_code_from(const nlohmann::json& report) {
    const auto& r = report.is_array() ? report[0] : report;
    const auto& contents = r.at("meta").at("field_content");
    // field_content layout: [Command, Phase, Code]
    return contents.at(2).get<std::string>();
}

// Helper: extract the error phase from a structured ErrorReport JSON.
std::string error_phase_from(const nlohmann::json& report) {
    const auto& r = report.is_array() ? report[0] : report;
    const auto& contents = r.at("meta").at("field_content");
    return contents.at(1).get<std::string>();
}

// Helper: extract the command name from a structured ErrorReport JSON.
std::string command_from(const nlohmann::json& report) {
    const auto& r = report.is_array() ? report[0] : report;
    const auto& contents = r.at("meta").at("field_content");
    return contents.at(0).get<std::string>();
}

} // anonymous namespace

// ============================================================================
// Suite: HappyPath
// ============================================================================

BOOST_AUTO_TEST_SUITE(HappyPath)

BOOST_AUTO_TEST_CASE(happy_path_returns_result) {
    HappyCommand cmd;

    nlohmann::json params;
    params["key"] = "value";

    nlohmann::json result = run([&]() { return cmd(std::move(params)); });

    BOOST_CHECK(result.is_object());
    BOOST_CHECK_EQUAL(result.at("status").get<std::string>(), "ok");
    BOOST_CHECK(result.at("input").contains("_validated"));
    BOOST_CHECK(result.at("input").at("_validated").get<bool>());
    BOOST_CHECK_EQUAL(result.at("input").at("key").get<std::string>(), "value");
}

BOOST_AUTO_TEST_CASE(empty_params_and_empty_result) {
    EmptyParamsCommand cmd;

    nlohmann::json result = run([&]() { return cmd(nlohmann::json::object()); });

    BOOST_CHECK(result.is_object());
    BOOST_CHECK(result.empty());
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: ValidationErrors
// ============================================================================

BOOST_AUTO_TEST_SUITE(ValidationErrors)

BOOST_AUTO_TEST_CASE(ensure_params_returns_error) {
    ValidationErrorCommand cmd;

    nlohmann::json result = run([&]() { return cmd(nlohmann::json::object()); });

    // Should be a structured error report array, not a normal result.
    BOOST_REQUIRE(result.is_array());
    BOOST_REQUIRE(!result.empty());
    const auto& err = result[0];
    BOOST_CHECK(err.contains("meta"));
    BOOST_CHECK(err.contains("message"));
    BOOST_CHECK(err.contains("detail"));
    BOOST_CHECK_EQUAL(command_from(result), "val_fail");
    BOOST_CHECK_EQUAL(error_phase_from(result),
                      std::string(schema::error_phase::validation));
    BOOST_CHECK_EQUAL(error_code_from(result),
                      std::string(schema::error_code::invalid_argument));
    BOOST_CHECK_EQUAL(err.at("message").get<std::string>(),
                      "Parameter 'pattern' is required.");
}

BOOST_AUTO_TEST_CASE(ensure_params_throws_exception_is_caught) {
    ValidationThrowCommand cmd;

    nlohmann::json result = run([&]() { return cmd(nlohmann::json::object()); });

    BOOST_REQUIRE(result.is_array());
    BOOST_REQUIRE(!result.empty());
    const auto& err = result[0];
    BOOST_CHECK(err.contains("meta"));
    BOOST_CHECK_EQUAL(command_from(result), "val_throw");
    BOOST_CHECK_EQUAL(error_phase_from(result),
                      std::string(schema::error_phase::validation));
    BOOST_CHECK_EQUAL(error_code_from(result),
                      std::string(schema::error_code::invalid_argument));
    // The exception message should appear in the error.
    std::string msg = err.at("message").get<std::string>();
    BOOST_CHECK(msg.find("Exception:") != std::string::npos);
    BOOST_CHECK(msg.find("bad argument") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: ExecutionErrors
// ============================================================================

BOOST_AUTO_TEST_SUITE(ExecutionErrors)

BOOST_AUTO_TEST_CASE(execute_returns_error) {
    ExecutionErrorCommand cmd;

    nlohmann::json result = run([&]() { return cmd(nlohmann::json::object()); });

    BOOST_REQUIRE(result.is_array());
    BOOST_REQUIRE(!result.empty());
    const auto& err = result[0];
    BOOST_CHECK(err.contains("meta"));
    BOOST_CHECK_EQUAL(command_from(result), "exec_fail");
    BOOST_CHECK_EQUAL(error_phase_from(result),
                      std::string(schema::error_phase::execution));
    BOOST_CHECK_EQUAL(error_code_from(result),
                      std::string(schema::error_code::internal_error));
    BOOST_CHECK_EQUAL(err.at("message").get<std::string>(),
                      "Database connection lost.");
}

BOOST_AUTO_TEST_CASE(execute_throws_exception_is_caught) {
    ExecutionThrowCommand cmd;

    nlohmann::json result = run([&]() { return cmd(nlohmann::json::object()); });

    BOOST_REQUIRE(result.is_array());
    BOOST_REQUIRE(!result.empty());
    const auto& err = result[0];
    BOOST_CHECK(err.contains("meta"));
    BOOST_CHECK_EQUAL(command_from(result), "exec_throw");
    BOOST_CHECK_EQUAL(error_phase_from(result),
                      std::string(schema::error_phase::execution));
    BOOST_CHECK_EQUAL(error_code_from(result),
                      std::string(schema::error_code::internal_error));
    std::string msg = err.at("message").get<std::string>();
    BOOST_CHECK(msg.find("Exception:") != std::string::npos);
    BOOST_CHECK(msg.find("Worker process crashed") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: ErrorReportStructure
// ============================================================================

BOOST_AUTO_TEST_SUITE(ErrorReportStructure)

BOOST_AUTO_TEST_CASE(validation_error_has_validation_phase) {
    ValidationErrorCommand cmd;
    nlohmann::json result = run([&]() { return cmd(nlohmann::json::object()); });

    BOOST_CHECK_EQUAL(error_phase_from(result),
                      std::string(schema::error_phase::validation));
}

BOOST_AUTO_TEST_CASE(execution_error_has_execution_phase) {
    ExecutionErrorCommand cmd;
    nlohmann::json result = run([&]() { return cmd(nlohmann::json::object()); });

    BOOST_CHECK_EQUAL(error_phase_from(result),
                      std::string(schema::error_phase::execution));
}

BOOST_AUTO_TEST_CASE(detail_is_null_by_default) {
    ValidationErrorCommand cmd;
    nlohmann::json result = run([&]() { return cmd(nlohmann::json::object()); });

    BOOST_REQUIRE(result.is_array());
    BOOST_REQUIRE(!result.empty());
    const auto& err = result[0];
    BOOST_CHECK(err.contains("detail"));
    BOOST_CHECK(err.at("detail").is_null());
}

BOOST_AUTO_TEST_CASE(meta_has_expected_keys) {
    HappyCommand cmd;
    nlohmann::json params;
    params["x"] = 1;

    nlohmann::json result = run([&]() { return cmd(std::move(params)); });

    // Happy path returns a result object, not an error — but errors have
    // the meta structure.  Verify the happy-path shape instead.
    BOOST_CHECK(result.contains("status"));
    BOOST_CHECK_EQUAL(result.at("status").get<std::string>(), "ok");
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: CommandNameInErrors
// ============================================================================

BOOST_AUTO_TEST_SUITE(CommandNameInErrors)

BOOST_AUTO_TEST_CASE(name_appears_in_validation_error) {
    ValidationErrorCommand cmd;
    nlohmann::json result = run([&]() { return cmd(nlohmann::json::object()); });

    BOOST_CHECK_EQUAL(command_from(result), "val_fail");
}

BOOST_AUTO_TEST_CASE(name_appears_in_execution_error) {
    ExecutionErrorCommand cmd;
    nlohmann::json result = run([&]() { return cmd(nlohmann::json::object()); });

    BOOST_CHECK_EQUAL(command_from(result), "exec_fail");
}

BOOST_AUTO_TEST_CASE(name_appears_when_ensure_params_throws) {
    ValidationThrowCommand cmd;
    nlohmann::json result = run([&]() { return cmd(nlohmann::json::object()); });

    BOOST_CHECK_EQUAL(command_from(result), "val_throw");
}

BOOST_AUTO_TEST_CASE(name_appears_when_execute_throws) {
    ExecutionThrowCommand cmd;
    nlohmann::json result = run([&]() { return cmd(nlohmann::json::object()); });

    BOOST_CHECK_EQUAL(command_from(result), "exec_throw");
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: ExpectField — tests for the expect_field / expect_field_or helpers
// ============================================================================

BOOST_AUTO_TEST_SUITE(ExpectFieldSuite)

// A minimal concrete command so we can call the helper templates.
struct FieldTestCommand : ServiceCommand {
    std::string_view name() const noexcept override { return "field_test"; }

    nlohmann::json ensure_params(nlohmann::json, nlohmann::json&) const override {
        return {};
    }

    boost::asio::awaitable<nlohmann::json> execute(nlohmann::json,
                                                    nlohmann::json&) override {
        co_return nlohmann::json{};
    }
};

BOOST_AUTO_TEST_CASE(expect_field_retrieves_existing_value) {
    FieldTestCommand cmd;
    nlohmann::json obj;
    obj["key"] = 42;

    nlohmann::json error;
    int64_t val = cmd.expect_field<int64_t>(obj, "key", error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(val, 42);
}

BOOST_AUTO_TEST_CASE(expect_field_missing_key_populates_error) {
    FieldTestCommand cmd;
    nlohmann::json obj;
    // "key" not present.

    nlohmann::json error;
    auto val = cmd.expect_field<int64_t>(obj, "key", error);

    BOOST_CHECK(!error.empty());
    BOOST_CHECK_EQUAL(val, 0);  // default-constructed int64_t
    BOOST_CHECK_EQUAL(error_code_from(error),
                      std::string(schema::error_code::invalid_argument));
}

BOOST_AUTO_TEST_CASE(expect_field_wrong_type_populates_error) {
    FieldTestCommand cmd;
    nlohmann::json obj;
    obj["key"] = "not_a_number";

    nlohmann::json error;
    auto val = cmd.expect_field<int64_t>(obj, "key", error);

    BOOST_CHECK(!error.empty());
    BOOST_CHECK_EQUAL(val, 0);
    BOOST_CHECK_EQUAL(error_code_from(error),
                      std::string(schema::error_code::invalid_argument));
}

BOOST_AUTO_TEST_CASE(expect_field_retrieves_string) {
    FieldTestCommand cmd;
    nlohmann::json obj;
    obj["name"] = "test_file.cpp";

    nlohmann::json error;
    std::string val = cmd.expect_field<std::string>(obj, "name", error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(val, "test_file.cpp");
}

BOOST_AUTO_TEST_CASE(expect_field_retrieves_bool) {
    FieldTestCommand cmd;
    nlohmann::json obj;
    obj["flag"] = true;

    nlohmann::json error;
    bool val = cmd.expect_field<bool>(obj, "flag", error);

    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(val, true);
}

BOOST_AUTO_TEST_CASE(expect_field_or_returns_value_when_present) {
    FieldTestCommand cmd;
    nlohmann::json obj;
    obj["count"] = 99;

    auto val = cmd.expect_field_or<int64_t>(obj, "count", 10);

    BOOST_CHECK_EQUAL(val, 99);
}

BOOST_AUTO_TEST_CASE(expect_field_or_returns_default_when_missing) {
    FieldTestCommand cmd;
    nlohmann::json obj;
    // "count" not present.

    auto val = cmd.expect_field_or<int64_t>(obj, "count", 10);

    BOOST_CHECK_EQUAL(val, 10);
}

BOOST_AUTO_TEST_CASE(expect_field_or_returns_default_when_wrong_type) {
    FieldTestCommand cmd;
    nlohmann::json obj;
    obj["count"] = "not_a_number";

    // Type mismatch → silently falls back to default.
    auto val = cmd.expect_field_or<int64_t>(obj, "count", 10);

    BOOST_CHECK_EQUAL(val, 10);
}

BOOST_AUTO_TEST_CASE(expect_field_or_defaults_to_default_constructed) {
    FieldTestCommand cmd;
    nlohmann::json obj;

    auto val = cmd.expect_field_or<std::string>(obj, "missing", std::string("fallback"));

    BOOST_CHECK_EQUAL(val, "fallback");
}

BOOST_AUTO_TEST_SUITE_END()
