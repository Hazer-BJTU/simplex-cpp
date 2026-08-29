#define BOOST_TEST_MODULE ProcessIoTests
#include <boost/test/unit_test.hpp>

#include "dataclass/process_io.hpp"

#include <nlohmann/json.hpp>

// Round-trip and contract tests for the process_io records: keys match
// fields, optionals omit when empty and never round-trip null, enums speak
// the legacy state vocabulary with Unknown as the fail-safe, unknown keys
// are ignored, and defaults survive an empty object. Pure data checks.

BOOST_AUTO_TEST_CASE(spec_round_trips_with_all_fields)
{
    const process_io::ProcessSpec spec{
        .description = "count lines",
        .executable = "wc",
        .arguments = {"-l", "notes.txt"},
        .timeout_milliseconds = std::uint64_t{1500},
        .kill_on_timeout = false,
        .extras = nlohmann::json{{"cwd", "/tmp"}},
    };

    const nlohmann::json j = spec;
    const process_io::ProcessSpec back = j.get<process_io::ProcessSpec>();

    BOOST_TEST(back.description == spec.description);
    BOOST_TEST(back.executable == spec.executable);
    BOOST_TEST(back.arguments == spec.arguments);
    // Boost.Test cannot print std::optional, so engaged values are asserted
    // through their contents, never as optionals.
    BOOST_TEST(back.timeout_milliseconds.has_value());
    BOOST_TEST(*back.timeout_milliseconds == std::uint64_t{1500});
    BOOST_TEST(back.kill_on_timeout == spec.kill_on_timeout);
    BOOST_TEST(back.extras.has_value());
    BOOST_TEST(*back.extras == *spec.extras);
    // Engaged optionals are present on the wire with the field's own key.
    BOOST_TEST(j.at("timeout_milliseconds") == nlohmann::json(1500));
    BOOST_TEST(j.at("kill_on_timeout") == nlohmann::json(false));
}

BOOST_AUTO_TEST_CASE(spec_defaults_when_optionals_empty)
{
    const process_io::ProcessSpec spec{
        .description = "fire and forget",
        .executable = "sleep",
        .arguments = {"10"},
    };

    const nlohmann::json j = spec;
    // Empty optionals are OMITTED, never written as null.
    BOOST_TEST(!j.contains("timeout_milliseconds"));
    BOOST_TEST(!j.contains("extras"));
    // kill_on_timeout has a meaningful default and is always written.
    BOOST_TEST(j.at("kill_on_timeout") == nlohmann::json(true));

    const process_io::ProcessSpec back = j.get<process_io::ProcessSpec>();
    BOOST_TEST(!back.timeout_milliseconds.has_value());
    BOOST_TEST(!back.extras.has_value());
    BOOST_TEST(back.kill_on_timeout == true);
}

BOOST_AUTO_TEST_CASE(execution_round_trips_exit_code_and_state)
{
    const process_io::ProcessExecution execution{
        .state = process_io::ProcessState::Exited,
        .exit_code = 3,
        .execution_milliseconds = std::uint64_t{421},
    };

    const nlohmann::json j = execution;
    BOOST_TEST(j.at("state") == nlohmann::json("exited"));
    BOOST_TEST(j.at("exit_code") == nlohmann::json(3));

    const process_io::ProcessExecution back =
        j.get<process_io::ProcessExecution>();
    BOOST_CHECK(back.state == process_io::ProcessState::Exited);
    BOOST_TEST(back.exit_code.has_value());
    BOOST_TEST(*back.exit_code == 3);
    BOOST_TEST(back.execution_milliseconds == execution.execution_milliseconds);
}

BOOST_AUTO_TEST_CASE(execution_defaults_are_unknown_and_pending)
{
    const process_io::ProcessExecution execution{};
    const nlohmann::json j = execution;
    // No exit yet: the key is omitted rather than written as null (the
    // legacy JSON wrote null — a presentation choice this contract drops).
    BOOST_TEST(!j.contains("exit_code"));

    const process_io::ProcessExecution back =
        nlohmann::json::object().get<process_io::ProcessExecution>();
    BOOST_CHECK(back.state == process_io::ProcessState::Unknown);
    BOOST_TEST(!back.exit_code.has_value());
    BOOST_TEST(back.execution_milliseconds == std::uint64_t{0});
}

BOOST_AUTO_TEST_CASE(report_round_trips_nested_execution_and_streams)
{
    const process_io::ProcessReport report{
        .id = std::uint64_t{7},
        .description = "echo hello",
        .execution =
            process_io::ProcessExecution{
                .state = process_io::ProcessState::Finished,
                .exit_code = 0,
                .execution_milliseconds = std::uint64_t{12},
            },
        .stdout_text = std::string("hello\n"),
        .stderr_text = std::string(""),
    };

    const nlohmann::json j = report;
    BOOST_TEST(j.at("id") == nlohmann::json(7));
    BOOST_TEST(j.at("execution").at("state") == nlohmann::json("finished"));
    BOOST_TEST(j.at("stdout_text") == nlohmann::json("hello\n"));
    // An engaged-but-empty stream stays engaged: "" means captured silence,
    // absent means "not part of this report".
    BOOST_TEST(j.at("stderr_text") == nlohmann::json(""));

    const process_io::ProcessReport back = j.get<process_io::ProcessReport>();
    BOOST_TEST(back.id == report.id);
    BOOST_TEST(back.description == report.description);
    BOOST_CHECK(back.execution.state == process_io::ProcessState::Finished);
    BOOST_TEST(back.execution.exit_code.has_value());
    BOOST_TEST(*back.execution.exit_code == 0);
    BOOST_TEST(back.stdout_text.has_value());
    BOOST_TEST(*back.stdout_text == std::string("hello\n"));
    BOOST_TEST(back.stderr_text.has_value());
    BOOST_TEST(*back.stderr_text == std::string(""));
}

BOOST_AUTO_TEST_CASE(report_meta_only_shape_omits_stream_keys)
{
    // A status listing carries identity + execution but no streams.
    const process_io::ProcessReport report{
        .id = std::uint64_t{2},
        .description = "sleep 30",
        .execution =
            process_io::ProcessExecution{
                .state = process_io::ProcessState::Running,
            },
    };

    const nlohmann::json j = report;
    BOOST_TEST(!j.contains("stdout_text"));
    BOOST_TEST(!j.contains("stderr_text"));
    BOOST_TEST(!j.contains("extras"));
}

BOOST_AUTO_TEST_CASE(null_and_absent_optionals_read_as_disengaged)
{
    // JSON null reads as ABSENT: an engaged optional must never hold null or
    // the never-null round-trip would break (detail::read_optional rule).
    const nlohmann::json j = nlohmann::json{
        {"id", 1},
        {"description", "x"},
        {"execution", nullptr},
        {"stdout_text", nullptr},
        {"timeout_milliseconds", nullptr},
        {"extras", nullptr},
    };

    const process_io::ProcessReport report =
        j.get<process_io::ProcessReport>();
    BOOST_TEST(!report.stdout_text.has_value());
    BOOST_TEST(!report.extras.has_value());
}

BOOST_AUTO_TEST_CASE(unknown_state_string_falls_back_to_unknown)
{
    // The fail-safe value is the first entry of the enum table: a value this
    // build does not recognise degrades to "I don't know", not to a guess.
    const nlohmann::json j = nlohmann::json{
        {"state", "hibernating"},
        {"exit_code", 0},
    };
    const process_io::ProcessExecution execution =
        j.get<process_io::ProcessExecution>();
    BOOST_CHECK(execution.state == process_io::ProcessState::Unknown);
    BOOST_TEST(execution.exit_code.value() == 0);
}

BOOST_AUTO_TEST_CASE(unknown_keys_are_ignored)
{
    const nlohmann::json j = nlohmann::json{
        {"id", 5},
        {"description", "cat"},
        {"execution", nlohmann::json::object()},
        {"stdout_text", "meow"},
        {"future_field", "dropped on read"},
    };

    const process_io::ProcessReport report = j.get<process_io::ProcessReport>();
    BOOST_TEST(report.id == std::uint64_t{5});
    BOOST_TEST(report.stdout_text.value() == "meow");
}
