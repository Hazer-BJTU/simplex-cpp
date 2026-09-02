#define BOOST_TEST_MODULE ProcessExceptionTests
#include <boost/test/unit_test.hpp>

#include "process/process_exceptions.hpp"

#include <boost/system/errc.hpp>

BOOST_AUTO_TEST_CASE(retains_failure_context)
{
    const auto ec = make_error_code(boost::system::errc::no_such_file_or_directory);
    const process::ProcessException exception(
        process::ProcessException::Stage::ResolveExecutable,
        "no such file",
        ec,
        "definitely-not-an-executable",
        "count lines");

    BOOST_CHECK(
        exception.stage() == process::ProcessException::Stage::ResolveExecutable);
    // what() carries the full rendering, not the bare message: a host that
    // can only catch std::exception (e.g. across a dlopen boundary) still
    // sees the whole context.
    BOOST_TEST(exception.what() == exception.to_string());
    BOOST_TEST(exception.error_code() == ec);
    BOOST_TEST(exception.executable() == "definitely-not-an-executable");
    BOOST_TEST(exception.description() == "count lines");
}

BOOST_AUTO_TEST_CASE(optional_context_defaults_to_empty)
{
    const process::ProcessException exception(
        process::ProcessException::Stage::Unknown, "unknown failure");

    BOOST_TEST(!exception.error_code());
    BOOST_TEST(exception.executable().empty());
    BOOST_TEST(exception.description().empty());
    BOOST_TEST(exception.to_string() ==
               std::string("Failed at an unknown stage: unknown failure"));
}

BOOST_AUTO_TEST_CASE(to_string_renders_failure_context)
{
    const auto ec = make_error_code(boost::system::errc::permission_denied);
    const process::ProcessException exception(
        process::ProcessException::Stage::Spawn,
        "fork failed",
        ec,
        "/usr/bin/wc",
        "count lines");

    // The expected ec text comes from the same error_code, so the assertion
    // stays locale-independent.
    BOOST_TEST(exception.to_string() ==
               "Failed while spawning the process: fork failed (" + ec.message() +
                   "; executable /usr/bin/wc; count lines)");
}

BOOST_AUTO_TEST_CASE(spawn_failure_shape_omits_absent_launch_context)
{
    // A spawn failure may carry the error code but no description yet —
    // absent fields are omitted from the parenthetical entirely.
    const auto ec = make_error_code(boost::system::errc::no_child_process);
    const process::ProcessException exception(
        process::ProcessException::Stage::Spawn, "child died", ec, "sleep");

    BOOST_TEST(exception.to_string() ==
               "Failed while spawning the process: child died (" + ec.message() +
                   "; executable sleep)");
}

BOOST_AUTO_TEST_CASE(catchable_as_std_exception)
{
    // The one dependable catch across a dlopen boundary — what() must carry
    // the whole rendering for exactly this consumer.
    try {
        throw process::ProcessException(
            process::ProcessException::Stage::Terminate,
            "SIGTERM ignored",
            {},
            "sleep",
            "cleanup sweep");
    } catch (const std::exception& caught) {
        BOOST_TEST(caught.what() ==
                   std::string("Failed while terminating the process: "
                               "SIGTERM ignored (executable sleep; cleanup "
                               "sweep)"));
    }
}

BOOST_AUTO_TEST_CASE(stage_phrase_covers_every_stage)
{
    using Stage = process::ProcessException::Stage;
    BOOST_TEST(process::ProcessException::stage_phrase(Stage::ResolveExecutable)
               == std::string_view("while resolving the executable"));
    BOOST_TEST(process::ProcessException::stage_phrase(Stage::Environment)
               == std::string_view("while assembling the child environment"));
    BOOST_TEST(process::ProcessException::stage_phrase(Stage::Spawn)
               == std::string_view("while spawning the process"));
    BOOST_TEST(process::ProcessException::stage_phrase(Stage::Terminate)
               == std::string_view("while terminating the process"));
    BOOST_TEST(process::ProcessException::stage_phrase(Stage::Write)
               == std::string_view("while writing to the process"));
    BOOST_TEST(process::ProcessException::stage_phrase(Stage::Read)
               == std::string_view("while reading from the process"));
    BOOST_TEST(process::ProcessException::stage_phrase(Stage::Unknown)
               == std::string_view("at an unknown stage"));
}
