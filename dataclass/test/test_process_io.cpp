#define BOOST_TEST_MODULE ProcessIoTests
#include <boost/test/unit_test.hpp>

#include "dataclass/process_spec.hpp"

#include <cstdint>
#include <sys/types.h>
#include <unordered_map>

#include <nlohmann/json.hpp>

// Round-trip and contract tests for the process_io records: keys match
// fields, optionals omit when empty and never round-trip null, enums speak
// the legacy state vocabulary with Unknown as the fail-safe, unknown keys
// are ignored, and defaults survive an empty object. Pure data checks.

BOOST_AUTO_TEST_CASE(launch_spec_round_trips_with_all_fields)
{
    const process_io::LaunchSpec spec{
        .executable = "wc",
        .arguments = {"-l", "notes.txt"},
        .description = "count lines",
        .pid = pid_t{4212},
        .timeout_milliseconds = std::uint64_t{1500},
        .detach_on_timeout = false,
        .environment = std::unordered_map<std::string, std::string>{
            {"PATH", "/usr/bin"},
            {"LANG", "C"},
        },
    };

    const nlohmann::json j = spec;
    const process_io::LaunchSpec back = j.get<process_io::LaunchSpec>();

    BOOST_TEST(back.executable == spec.executable);
    BOOST_TEST(back.arguments == spec.arguments);
    BOOST_TEST(back.description == spec.description);
    BOOST_TEST(back.timeout_milliseconds == spec.timeout_milliseconds);
    BOOST_TEST(back.detach_on_timeout == spec.detach_on_timeout);
    BOOST_TEST(back.inherit_environment == spec.inherit_environment);
    // Boost.Test cannot print std::optional, so engaged values are asserted
    // through their contents, never as optionals.
    BOOST_TEST(back.pid.has_value());
    BOOST_TEST(*back.pid == pid_t{4212});
    BOOST_TEST(back.environment.has_value());
    BOOST_TEST(*back.environment == *spec.environment);
    // The timeout is a required field: always on the wire. Booleans carry
    // their meaningful defaults the same way.
    BOOST_TEST(j.at("timeout_milliseconds") == nlohmann::json(1500));
    BOOST_TEST(j.at("detach_on_timeout") == nlohmann::json(false));
    BOOST_TEST(j.at("inherit_environment") == nlohmann::json(true));
    BOOST_TEST(j.at("pid") == nlohmann::json(4212));
    BOOST_TEST(j.at("environment").at("PATH") == nlohmann::json("/usr/bin"));
}

BOOST_AUTO_TEST_CASE(launch_spec_defaults_write_required_fields)
{
    const process_io::LaunchSpec spec{
        .executable = "sleep",
        .arguments = {"10"},
        .description = "nap",
    };

    const nlohmann::json j = spec;
    // timeout_milliseconds and the two switches are required/meaningful
    // fields and are always written; only optionals may be omitted.
    BOOST_TEST(j.at("timeout_milliseconds") == nlohmann::json(0));
    BOOST_TEST(j.at("detach_on_timeout") == nlohmann::json(true));
    BOOST_TEST(j.at("inherit_environment") == nlohmann::json(true));
    BOOST_TEST(!j.contains("pid"));
    BOOST_TEST(!j.contains("environment"));

    const process_io::LaunchSpec back = j.get<process_io::LaunchSpec>();
    BOOST_TEST(back.timeout_milliseconds == std::uint64_t{0});
    BOOST_TEST(back.detach_on_timeout == true);
    BOOST_TEST(back.inherit_environment == true);
    BOOST_TEST(!back.pid.has_value());
    BOOST_TEST(!back.environment.has_value());
}

BOOST_AUTO_TEST_CASE(launch_spec_engaged_empty_environment_stays_engaged)
{
    const process_io::LaunchSpec spec{
        .executable = "env",
        .arguments = {},
        .description = "no extra entries",
        .timeout_milliseconds = std::uint64_t{100},
        .environment = std::unordered_map<std::string, std::string>{},
    };

    const nlohmann::json j = spec;
    // An engaged-but-empty map stays engaged: {} means "explicitly no extra
    // entries", absent means "no environment section in this spec at all".
    BOOST_TEST(j.at("environment") == nlohmann::json::object());

    const process_io::LaunchSpec back = j.get<process_io::LaunchSpec>();
    BOOST_TEST(back.environment.has_value());
    BOOST_TEST(back.environment->empty());
}

BOOST_AUTO_TEST_CASE(execution_status_round_trips_exit_code_and_state)
{
    const process_io::ExecutionStatus status{
        .state = process_io::ProcessState::Exited,
        .exit_code = 3,
        .cumulative_execution_milliseconds = std::uint64_t{421},
    };

    const nlohmann::json j = status;
    BOOST_TEST(j.at("state") == nlohmann::json("exited"));
    BOOST_TEST(j.at("exit_code") == nlohmann::json(3));
    BOOST_TEST(j.at("cumulative_execution_milliseconds") ==
               nlohmann::json(421));

    const process_io::ExecutionStatus back =
        j.get<process_io::ExecutionStatus>();
    BOOST_CHECK(back.state == process_io::ProcessState::Exited);
    BOOST_TEST(back.exit_code.has_value());
    BOOST_TEST(*back.exit_code == 3);
    BOOST_TEST(back.cumulative_execution_milliseconds ==
               status.cumulative_execution_milliseconds);
}

BOOST_AUTO_TEST_CASE(execution_status_defaults_are_unknown_and_pending)
{
    const process_io::ExecutionStatus status{};
    const nlohmann::json j = status;
    // No exit yet: the key is omitted rather than written as null (the
    // legacy JSON wrote null — a presentation choice this contract drops).
    BOOST_TEST(!j.contains("exit_code"));

    const process_io::ExecutionStatus back =
        nlohmann::json::object().get<process_io::ExecutionStatus>();
    BOOST_CHECK(back.state == process_io::ProcessState::Unknown);
    BOOST_TEST(!back.exit_code.has_value());
    BOOST_TEST(back.cumulative_execution_milliseconds == std::uint64_t{0});
}

BOOST_AUTO_TEST_CASE(result_round_trips_spec_status_and_streams)
{
    const process_io::ExecutionResult result{
        .spec =
            process_io::LaunchSpec{
                .executable = "echo",
                .arguments = {"hello"},
                .description = "echo hello",
                .pid = pid_t{7},
                .timeout_milliseconds = std::uint64_t{500},
            },
        .execution =
            process_io::ExecutionStatus{
                .state = process_io::ProcessState::Exited,
                .exit_code = 0,
                .cumulative_execution_milliseconds = std::uint64_t{12},
            },
        .stdout_text = std::string("hello\n"),
        .stderr_text = std::string(""),
    };

    const nlohmann::json j = result;
    // spec + pid is the identity now: no separate id/description keys.
    BOOST_TEST(j.at("spec").at("pid") == nlohmann::json(7));
    BOOST_TEST(j.at("spec").at("executable") == nlohmann::json("echo"));
    BOOST_TEST(!j.contains("id"));
    BOOST_TEST(!j.contains("description"));
    BOOST_TEST(j.at("execution").at("state") == nlohmann::json("exited"));
    BOOST_TEST(j.at("stdout_text") == nlohmann::json("hello\n"));
    // An engaged-but-empty stream stays engaged: "" means captured silence,
    // absent means "not part of this report".
    BOOST_TEST(j.at("stderr_text") == nlohmann::json(""));

    const process_io::ExecutionResult back =
        j.get<process_io::ExecutionResult>();
    BOOST_TEST(back.spec.executable == result.spec.executable);
    BOOST_TEST(back.spec.arguments == result.spec.arguments);
    BOOST_TEST(back.spec.description == result.spec.description);
    BOOST_TEST(back.spec.pid.has_value());
    BOOST_TEST(*back.spec.pid == pid_t{7});
    BOOST_TEST(back.spec.timeout_milliseconds == std::uint64_t{500});
    BOOST_CHECK(back.execution.state == process_io::ProcessState::Exited);
    BOOST_TEST(back.execution.exit_code.has_value());
    BOOST_TEST(*back.execution.exit_code == 0);
    BOOST_TEST(back.execution.cumulative_execution_milliseconds ==
               std::uint64_t{12});
    BOOST_TEST(back.stdout_text.has_value());
    BOOST_TEST(*back.stdout_text == std::string("hello\n"));
    BOOST_TEST(back.stderr_text.has_value());
    BOOST_TEST(*back.stderr_text == std::string(""));
}

BOOST_AUTO_TEST_CASE(result_meta_only_shape_omits_stream_keys)
{
    // A status listing carries spec + execution but no streams.
    const process_io::ExecutionResult result{
        .spec =
            process_io::LaunchSpec{
                .executable = "sleep",
                .arguments = {"30"},
                .description = "sleep 30",
                .pid = pid_t{99},
                .timeout_milliseconds = std::uint64_t{30000},
            },
        .execution =
            process_io::ExecutionStatus{
                .state = process_io::ProcessState::Running,
            },
    };

    const nlohmann::json j = result;
    BOOST_TEST(!j.contains("stdout_text"));
    BOOST_TEST(!j.contains("stderr_text"));
}

BOOST_AUTO_TEST_CASE(null_and_absent_optionals_read_as_disengaged)
{
    // JSON null reads as ABSENT: an engaged optional must never hold null or
    // the never-null round-trip would break (detail::read_optional rule).
    const nlohmann::json spec_j = nlohmann::json{
        {"executable", "sleep"},
        {"description", "x"},
        {"pid", nullptr},
        {"environment", nullptr},
    };
    const process_io::LaunchSpec spec = spec_j.get<process_io::LaunchSpec>();
    BOOST_TEST(!spec.pid.has_value());
    BOOST_TEST(!spec.environment.has_value());

    const nlohmann::json result_j = nlohmann::json{
        {"spec", nullptr},
        {"execution", nullptr},
        {"stdout_text", nullptr},
    };
    const process_io::ExecutionResult result =
        result_j.get<process_io::ExecutionResult>();
    BOOST_TEST(!result.spec.pid.has_value());
    BOOST_TEST(!result.stdout_text.has_value());
}

BOOST_AUTO_TEST_CASE(unknown_state_string_falls_back_to_unknown)
{
    // The fail-safe value is the first entry of the enum table: a value this
    // build does not recognise degrades to "I don't know", not to a guess.
    const nlohmann::json j = nlohmann::json{
        {"state", "hibernating"},
        {"exit_code", 0},
    };
    const process_io::ExecutionStatus status =
        j.get<process_io::ExecutionStatus>();
    BOOST_CHECK(status.state == process_io::ProcessState::Unknown);
    BOOST_TEST(status.exit_code.value() == 0);
}

BOOST_AUTO_TEST_CASE(unknown_keys_are_ignored)
{
    const nlohmann::json j = nlohmann::json{
        {"spec", nlohmann::json{{"executable", "cat"},
                                {"description", "meow"},
                                {"pid", 5}}},
        {"execution", nlohmann::json::object()},
        {"stdout_text", "meow"},
        {"future_field", "dropped on read"},
    };
    const process_io::ExecutionResult result =
        j.get<process_io::ExecutionResult>();
    BOOST_TEST(result.spec.executable == std::string("cat"));
    BOOST_TEST(result.spec.pid.value() == pid_t{5});
    BOOST_TEST(result.stdout_text.value() == "meow");
}
