#define BOOST_TEST_MODULE InvokeExceptionTests
#include <boost/test/unit_test.hpp>

#include "tools/invoke_exception.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/errc.hpp>

namespace asio = boost::asio;

namespace {

/// The invocation context every failure below is raised for: a tool call as the
/// conversation carries it, with the id that correlates the result back to the
/// model's call.
model_io::InvokeQuery read_file_query()
{
    model_io::InvokeQuery query;
    query.type = model_io::InvokeType::ReadOnly;
    query.security = model_io::InvokeSecurity::Trusted;
    query.id = "call_1";
    query.name = "read_file";
    query.arguments = {{"path", "/etc/hosts"}};
    query.extras = {{"provider", "local"}};
    return query;
}

/// The unified handler the InvokeReturn bridge exists for: an invocation that
/// runs detached — its result is a record handed back to the conversation, and
/// there is no caller left to throw at. A failure becomes that record.
asio::awaitable<model_io::InvokeReturn> invocation(bool tool_fails)
{
    const model_io::InvokeQuery query = read_file_query();
    try {
        if (tool_fails) {
            throw tools::InvokeException(
                tools::InvokeException::Stage::Invoke, "the tool failed", query);
        }
        model_io::InvokeReturn record;
        record.query = query;
        record.output.type = model_io::ContentType::Text;
        record.output.raw = "127.0.0.1 localhost";
        co_return record;
    } catch (const tools::InvokeException& failure) {
        co_return failure;   // implicit operator model_io::InvokeReturn()
    }
}

model_io::InvokeReturn drive(asio::awaitable<model_io::InvokeReturn> operation)
{
    asio::io_context io;
    auto pending = asio::co_spawn(io, std::move(operation), asio::use_future);
    io.run();
    return pending.get();   // rethrows a failure the handler did not catch
}

} // namespace

BOOST_AUTO_TEST_CASE(retains_stage_message_query_and_error_code)
{
    const auto ec = make_error_code(boost::system::errc::permission_denied);
    const model_io::InvokeQuery query = read_file_query();
    const tools::InvokeException failure(
        tools::InvokeException::Stage::SecurityCheck,
        "the tool is not allowed to run",
        query,
        ec);

    BOOST_CHECK(failure.stage() == tools::InvokeException::Stage::SecurityCheck);
    BOOST_TEST(failure.message() == "the tool is not allowed to run");
    BOOST_TEST(failure.error_code() == ec);
    BOOST_TEST(failure.query().id == "call_1");
    BOOST_TEST(failure.query().name == "read_file");
    // what() carries the full rendering, not the bare message: a host that can
    // only catch std::exception (e.g. across a dlopen boundary) still sees the
    // whole context.
    BOOST_TEST(failure.what() == failure.to_string());
}

BOOST_AUTO_TEST_CASE(what_renders_stage_phrase_message_and_context)
{
    const auto ec = make_error_code(boost::system::errc::permission_denied);
    const tools::InvokeException failure(
        tools::InvokeException::Stage::SecurityCheck,
        "the tool is not allowed to run",
        read_file_query(),
        ec);

    // The expected error-code text comes from the same error_code, so the
    // assertion stays locale-independent.
    BOOST_TEST(failure.to_string() ==
               "Failed while validating the invocation's security: "
               "the tool is not allowed to run (" +
                   ec.message() + "; tool read_file; call call_1)");
}

BOOST_AUTO_TEST_CASE(each_stage_renders_its_own_phrase)
{
    using Stage = tools::InvokeException::Stage;

    BOOST_TEST(tools::InvokeException(Stage::Dispatch, "no such tool \"write_file\"")
                   .to_string() ==
               "Failed while dispatching the invocation to a tool: "
               "no such tool \"write_file\"");
    BOOST_TEST(tools::InvokeException(Stage::ArgumentParse, "missing required property \"path\"")
                   .to_string() ==
               "Failed while parsing the invocation arguments: "
               "missing required property \"path\"");
    BOOST_TEST(tools::InvokeException(Stage::Invoke, "the tool failed").to_string() ==
               "Failed while invoking the tool: the tool failed");
    BOOST_TEST(tools::InvokeException(Stage::ResultCheck, "result is not valid JSON")
                   .to_string() ==
               "Failed while validating the tool result: result is not valid JSON");
    BOOST_TEST(tools::InvokeException(Stage::Unknown, "the handler gave up").to_string() ==
               "Failed at an unknown stage: the handler gave up");
}

BOOST_AUTO_TEST_CASE(a_failure_to_dispatch_names_its_own_stage)
{
    // The call never reached a tool: nothing was resolved to run, so the
    // failure is raised at Dispatch — before any security check, argument
    // parse, or invoke — and the marker says so.
    const tools::InvokeException failure(
        tools::InvokeException::Stage::Dispatch,
        "no such tool \"write_file\"",
        read_file_query());

    BOOST_CHECK(failure.stage() == tools::InvokeException::Stage::Dispatch);
    BOOST_TEST(failure.to_string() ==
               "Failed while dispatching the invocation to a tool: "
               "no such tool \"write_file\" (tool read_file; call call_1)");

    const model_io::InvokeReturn record = failure;
    BOOST_TEST(tools::is_error(record));
    BOOST_REQUIRE(record.extras.has_value());
    BOOST_TEST(record.extras->at("error").at("stage") == "dispatch");
    BOOST_TEST(record.extras->at("error").at("message") ==
               "no such tool \"write_file\"");
    BOOST_TEST(record.output.raw == failure.what());

    const auto stage = tools::error_stage(record);
    BOOST_REQUIRE(stage.has_value());
    BOOST_CHECK(*stage == tools::InvokeException::Stage::Dispatch);
}

BOOST_AUTO_TEST_CASE(absent_context_is_omitted_from_the_rendering)
{
    const auto ec = make_error_code(boost::system::errc::no_such_file_or_directory);

    // An error code without an invocation context.
    BOOST_TEST(tools::InvokeException(
                   tools::InvokeException::Stage::Invoke, "read failed", {}, ec)
                   .to_string() ==
               "Failed while invoking the tool: read failed (" + ec.message() + ")");

    // A context without an error code: a failure the tool reported itself.
    model_io::InvokeQuery named;
    named.name = "read_file";
    BOOST_TEST(tools::InvokeException(
                   tools::InvokeException::Stage::Invoke, "read failed", named)
                   .to_string() ==
               "Failed while invoking the tool: read failed (tool read_file)");

    model_io::InvokeQuery identified;
    identified.id = "call_9";
    BOOST_TEST(tools::InvokeException(
                   tools::InvokeException::Stage::Invoke, "read failed", identified)
                   .to_string() ==
               "Failed while invoking the tool: read failed (call call_9)");
}

BOOST_AUTO_TEST_CASE(the_query_is_owned_not_borrowed)
{
    // The failure outlives the frame that raised it — a detached invocation's
    // handler is the only owner left — so the context must be a copy.
    model_io::InvokeQuery scratch = read_file_query();
    const tools::InvokeException failure(
        tools::InvokeException::Stage::ArgumentParse, "bad arguments", scratch);

    scratch.name = "mutated";
    scratch.arguments["path"] = "/mutated";

    BOOST_TEST(failure.query().name == "read_file");
    BOOST_TEST(failure.query().arguments["path"] == "/etc/hosts");
}

BOOST_AUTO_TEST_CASE(a_copy_carries_the_whole_failure)
{
    // What a handler that keeps the failure instead of rethrowing it holds.
    const tools::InvokeException original(
        tools::InvokeException::Stage::ArgumentParse, "bad arguments",
        read_file_query());
    const tools::InvokeException copy = original;

    BOOST_CHECK(copy.stage() == original.stage());
    BOOST_TEST(copy.message() == original.message());
    BOOST_TEST(copy.what() == original.what());
    BOOST_TEST(nlohmann::json(copy.query()) == nlohmann::json(original.query()));
}

BOOST_AUTO_TEST_CASE(catchable_as_std_exception_with_the_full_rendering)
{
    std::string rendered;
    try {
        throw tools::InvokeException(
            tools::InvokeException::Stage::ResultCheck,
            "result is not valid JSON",
            read_file_query());
    } catch (const std::exception& failure) {
        rendered = failure.what();
    }

    BOOST_TEST(rendered ==
               "Failed while validating the tool result: "
               "result is not valid JSON (tool read_file; call call_1)");
}

BOOST_AUTO_TEST_CASE(to_invoke_return_carries_the_query_and_the_rendering)
{
    const tools::InvokeException failure(
        tools::InvokeException::Stage::ArgumentParse,
        "missing required property \"path\"",
        read_file_query());

    const model_io::InvokeReturn record = failure.to_invoke_return();

    // The query travels whole: it is what correlates the result to the model's
    // call and what a wire-level tool-result message needs.
    BOOST_TEST(nlohmann::json(record.query) == nlohmann::json(read_file_query()));
    // The prose the model reads.
    // BOOST_CHECK, like the rest of the tree's model_io enum assertions: a
    // scoped enum is not a printable operand for BOOST_TEST.
    BOOST_CHECK(record.output.type == model_io::ContentType::Text);
    BOOST_TEST(record.output.raw == failure.what());
    // The marker belongs to the record, not to the content part.
    BOOST_TEST(!record.output.extras.has_value());
}

BOOST_AUTO_TEST_CASE(the_failure_marker_names_the_stage_and_the_bare_message)
{
    const tools::InvokeException failure(
        tools::InvokeException::Stage::ArgumentParse,
        "missing required property \"path\"",
        read_file_query(),
        make_error_code(boost::system::errc::permission_denied));

    const model_io::InvokeReturn record = failure;

    BOOST_TEST(tools::InvokeException::extras_key == "error");
    BOOST_REQUIRE(record.extras.has_value());
    BOOST_TEST(record.extras->is_object());
    BOOST_TEST(record.extras->size() == 1u);

    const nlohmann::json marker = record.extras->at("error");
    BOOST_TEST(marker.is_object());
    // Exactly the two documented fields: bare message, not the rendering, and
    // no duplicate of the error code (that lives in error_code() and in the
    // rendered text the model gets).
    BOOST_TEST(marker.size() == 2u);
    BOOST_TEST(marker.at("stage") == "argument_parse");
    BOOST_TEST(marker.at("message") == "missing required property \"path\"");
}

BOOST_AUTO_TEST_CASE(the_marker_is_read_back_from_the_record)
{
    using Stage = tools::InvokeException::Stage;

    const tools::InvokeException failure(
        Stage::SecurityCheck, "the tool is not allowed to run", read_file_query());
    const model_io::InvokeReturn record = failure;

    BOOST_TEST(tools::is_error(record));
    const auto stage = tools::error_stage(record);
    BOOST_REQUIRE(stage.has_value());
    BOOST_CHECK(*stage == Stage::SecurityCheck);
}

BOOST_AUTO_TEST_CASE(the_marker_survives_a_json_round_trip)
{
    const tools::InvokeException failure(
        tools::InvokeException::Stage::ResultCheck, "result is not valid JSON",
        read_file_query());

    // The failure is persisted with the conversation it belongs to, so a
    // restored session classifies it exactly like the live one did.
    const nlohmann::json persisted = failure.to_invoke_return();
    const model_io::InvokeReturn restored =
        persisted.get<model_io::InvokeReturn>();

    BOOST_TEST(tools::is_error(restored));
    // Unwrapped rather than compared in place: a std::optional is not a
    // printable operand for BOOST_TEST.
    const auto restored_stage = tools::error_stage(restored);
    BOOST_REQUIRE(restored_stage.has_value());
    BOOST_CHECK(*restored_stage == tools::InvokeException::Stage::ResultCheck);
    BOOST_TEST(restored.output.raw == failure.what());
    BOOST_TEST(restored.extras->at("error").at("message") ==
               failure.message());
    BOOST_TEST(nlohmann::json(restored.query) ==
               nlohmann::json(failure.query()));
}

BOOST_AUTO_TEST_CASE(an_ordinary_return_is_not_an_error)
{
    BOOST_TEST(!tools::is_error(model_io::InvokeReturn{}));
    BOOST_TEST(!tools::error_stage(model_io::InvokeReturn{}).has_value());

    model_io::InvokeReturn unrelated_extras;
    unrelated_extras.extras = nlohmann::json{{"timing_ms", 12}};
    BOOST_TEST(!tools::is_error(unrelated_extras));
    BOOST_TEST(!tools::error_stage(unrelated_extras).has_value());

    // "error" present but not an object: not a marker.
    model_io::InvokeReturn wrong_marker_type;
    wrong_marker_type.extras = nlohmann::json{{"error", "read failed"}};
    BOOST_TEST(!tools::is_error(wrong_marker_type));
    BOOST_TEST(!tools::error_stage(wrong_marker_type).has_value());

    // extras is not an object at all.
    model_io::InvokeReturn extras_not_an_object;
    extras_not_an_object.extras = nlohmann::json::array({1, 2});
    BOOST_TEST(!tools::is_error(extras_not_an_object));
    BOOST_TEST(!tools::error_stage(extras_not_an_object).has_value());
}

BOOST_AUTO_TEST_CASE(a_marker_without_a_known_stage_still_marks_a_failure)
{
    // The record is still a failure — the marker is there — but the stage is
    // not one this build knows, so it does not guess one.
    model_io::InvokeReturn no_stage;
    no_stage.extras = nlohmann::json{{"error", {{"message", "read failed"}}}};
    BOOST_TEST(tools::is_error(no_stage));
    BOOST_TEST(!tools::error_stage(no_stage).has_value());

    model_io::InvokeReturn future_stage;
    future_stage.extras =
        nlohmann::json{{"error", {{"stage", "quota_exceeded"}}}};
    BOOST_TEST(tools::is_error(future_stage));
    BOOST_TEST(!tools::error_stage(future_stage).has_value());

    model_io::InvokeReturn non_string_stage;
    non_string_stage.extras =
        nlohmann::json{{"error", {{"stage", 3}}}};
    BOOST_TEST(tools::is_error(non_string_stage));
    BOOST_TEST(!tools::error_stage(non_string_stage).has_value());
}

BOOST_AUTO_TEST_CASE(stage_tokens_round_trip)
{
    using Stage = tools::InvokeException::Stage;

    BOOST_TEST(tools::InvokeException::stage_key(Stage::Dispatch) == "dispatch");
    BOOST_TEST(tools::InvokeException::stage_key(Stage::SecurityCheck) ==
               "security_check");
    BOOST_TEST(tools::InvokeException::stage_key(Stage::ArgumentParse) ==
               "argument_parse");
    BOOST_TEST(tools::InvokeException::stage_key(Stage::Invoke) == "invoke");
    BOOST_TEST(tools::InvokeException::stage_key(Stage::ResultCheck) ==
               "result_check");
    BOOST_TEST(tools::InvokeException::stage_key(Stage::Unknown) == "unknown");

    const Stage stages[] = {Stage::Dispatch, Stage::SecurityCheck,
                            Stage::ArgumentParse, Stage::Invoke,
                            Stage::ResultCheck, Stage::Unknown};
    for (const Stage stage : stages) {
        const auto parsed =
            tools::InvokeException::stage_from_key(
                tools::InvokeException::stage_key(stage));
        BOOST_REQUIRE(parsed.has_value());
        BOOST_CHECK(*parsed == stage);
        BOOST_CHECK(!tools::InvokeException::stage_phrase(stage).empty());
    }

    BOOST_TEST(!tools::InvokeException::stage_from_key("quota_exceeded")
                    .has_value());
}

BOOST_AUTO_TEST_CASE(a_detached_invocation_reports_its_failure_as_the_result)
{
    const model_io::InvokeReturn failed = drive(invocation(true));

    BOOST_TEST(tools::is_error(failed));
    const auto stage = tools::error_stage(failed);
    BOOST_REQUIRE(stage.has_value());
    BOOST_CHECK(*stage == tools::InvokeException::Stage::Invoke);
    BOOST_CHECK(failed.output.type == model_io::ContentType::Text);
    BOOST_TEST(failed.output.raw ==
               "Failed while invoking the tool: the tool failed "
               "(tool read_file; call call_1)");
    // The id is what the wire-level tool-result message is correlated by.
    BOOST_TEST(failed.query.id == "call_1");

    const model_io::InvokeReturn succeeded = drive(invocation(false));
    BOOST_TEST(!tools::is_error(succeeded));
    BOOST_TEST(!tools::error_stage(succeeded).has_value());
    BOOST_TEST(succeeded.output.raw == "127.0.0.1 localhost");
}
