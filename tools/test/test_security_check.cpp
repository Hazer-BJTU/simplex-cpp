#define BOOST_TEST_MODULE SecurityCheckTests
#include <boost/test/unit_test.hpp>

#include "tools/security_check.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <stdexcept>
#include <string>
#include <tuple>

namespace asio = boost::asio;

using tools::ConfirmDecision;
using tools::InvokeConfirmEvent;

namespace {

/// The settled call a policy is asked about: arguments ensured, and the
/// attributes write_attributes() writes already on it — which is what the
/// policy reads, so the security level is the whole input to the decision.
model_io::InvokeQuery settled_query(model_io::InvokeSecurity security)
{
    model_io::InvokeQuery query;
    query.type = model_io::InvokeType::SerialWrite;
    query.security = security;
    query.id = "call_1";
    query.name = "write_file";
    query.arguments = {{"path", "/tmp/out.txt"}};
    return query;
}

/// Run the policy on a bus the test owns and hand back its answer. The query
/// outlives the coroutine: it is alive until io.run() returns.
std::tuple<bool, std::string> check_on(
    model_io::InvokeQuery query, eventbus::AsyncEventBus& bus)
{
    asio::io_context io;
    auto pending = asio::co_spawn(
        io, tools::default_security_check(query, bus), asio::use_future);
    io.run();
    return pending.get(); // rethrows a throwing handler's exception
}

/// The same, through the default argument: the process-wide bus that a host in
/// another module — or in a dlopened plugin — would subscribe on, and the path
/// production code takes.
std::tuple<bool, std::string> check_on_default_bus(model_io::InvokeQuery query)
{
    asio::io_context io;
    auto pending = asio::co_spawn(
        io, tools::default_security_check(query), asio::use_future);
    io.run();
    return pending.get();
}

/// A confirmer that answers everything the same way, remembering what it saw.
struct AnsweringConfirmer {
    /// Built from the answer, so a test says only what it is about; `asked` and
    /// `seen` are the observations it fills in.
    AnsweringConfirmer(ConfirmDecision answer, std::string why)
        : decision(answer), reason(std::move(why))
    {}

    ConfirmDecision decision = ConfirmDecision::Approved;
    std::string reason;
    bool asked = false;
    model_io::InvokeQuery seen;

    boost::asio::awaitable<InvokeConfirmEvent> operator()(
        const InvokeConfirmEvent& request)
    {
        asked = true;
        seen = request.query;
        InvokeConfirmEvent out = request;
        out.decision = decision;
        out.reason = reason;
        co_return out;
    }
};

/// The handler to subscribe for `confirmer`, holding it by REFERENCE: the bus
/// copies a handler into its slot, so a directly-subscribed AnsweringConfirmer
/// would record `asked`/`seen` in a copy the test cannot see. The reference
/// stays valid because the confirmer outlives the subscription in every test
/// below.
auto answering(AnsweringConfirmer& confirmer)
{
    return [&confirmer](const InvokeConfirmEvent& request)
               -> asio::awaitable<InvokeConfirmEvent> {
        co_return co_await confirmer(request);
    };
}

} // namespace

BOOST_AUTO_TEST_CASE(the_request_starts_unanswered_and_carries_the_query)
{
    const model_io::InvokeQuery query =
        settled_query(model_io::InvokeSecurity::RequireConfirm);
    const InvokeConfirmEvent request = InvokeConfirmEvent::for_query(query);

    BOOST_CHECK(request.decision == ConfirmDecision::Unanswered);
    BOOST_TEST(request.reason.empty());
    BOOST_TEST(nlohmann::json(request.query) == nlohmann::json(query));
    // The bus's own metadata starts empty: nothing has been captured yet.
    BOOST_TEST(request.errors.empty());
}

BOOST_AUTO_TEST_CASE(trusted_passes_without_touching_the_bus)
{
    eventbus::AsyncEventBus bus;
    AnsweringConfirmer confirmer{ConfirmDecision::Denied, "denied by mistake"};
    auto subscription = bus.subscribe<InvokeConfirmEvent>(answering(confirmer));

    const auto [passed, reason] =
        check_on(settled_query(model_io::InvokeSecurity::Trusted), bus);

    BOOST_TEST(passed);
    BOOST_TEST(!reason.empty());
    // A trusted call is decided by its level alone: the confirmer that would
    // have denied it is never consulted.
    BOOST_TEST(!confirmer.asked);
}

BOOST_AUTO_TEST_CASE(default_deny_refuses_without_touching_the_bus)
{
    eventbus::AsyncEventBus bus;
    AnsweringConfirmer confirmer{ConfirmDecision::Approved, "approved by mistake"};
    auto subscription = bus.subscribe<InvokeConfirmEvent>(answering(confirmer));

    const auto [passed, reason] =
        check_on(settled_query(model_io::InvokeSecurity::DefaultDeny), bus);

    BOOST_TEST(!passed);
    BOOST_TEST(reason == "the invocation's security level is default deny, "
                         "so it was never cleared to run");
    // DefaultDeny is a decision, not a question: an approving confirmer cannot
    // turn it into a run either.
    BOOST_TEST(!confirmer.asked);
}

BOOST_AUTO_TEST_CASE(require_confirm_without_a_handler_refuses)
{
    eventbus::AsyncEventBus bus;
    BOOST_REQUIRE(bus.subscriber_count<InvokeConfirmEvent>() == 0u);

    const auto [passed, reason] =
        check_on(settled_query(model_io::InvokeSecurity::RequireConfirm), bus);

    // Fail closed: a request nobody can answer is a refusal, not a pass.
    BOOST_TEST(!passed);
    BOOST_TEST(reason == "no handler is subscribed to confirm the invocation");
}

BOOST_AUTO_TEST_CASE(an_approving_handler_lets_the_invocation_run)
{
    eventbus::AsyncEventBus bus;
    AnsweringConfirmer confirmer{ConfirmDecision::Approved, "the human said yes"};
    auto subscription = bus.subscribe<InvokeConfirmEvent>(answering(confirmer));

    const auto [passed, reason] =
        check_on(settled_query(model_io::InvokeSecurity::RequireConfirm), bus);

    BOOST_TEST(passed);
    // The handler's own words travel back as the (informational) reason.
    BOOST_TEST(reason == "the human said yes");
    // ... and it was asked about the settled call, not about a bare name.
    BOOST_TEST(confirmer.asked);
    BOOST_TEST(confirmer.seen.id == "call_1");
    BOOST_TEST(confirmer.seen.name == "write_file");
    BOOST_TEST(confirmer.seen.arguments["path"] == "/tmp/out.txt");
    BOOST_CHECK(confirmer.seen.security ==
                model_io::InvokeSecurity::RequireConfirm);
}

BOOST_AUTO_TEST_CASE(a_denying_handler_explains_the_refusal)
{
    eventbus::AsyncEventBus bus;
    AnsweringConfirmer confirmer{ConfirmDecision::Denied, "the human clicked Deny"};
    auto subscription = bus.subscribe<InvokeConfirmEvent>(answering(confirmer));

    const auto [passed, reason] =
        check_on(settled_query(model_io::InvokeSecurity::RequireConfirm), bus);

    BOOST_TEST(!passed);
    BOOST_TEST(reason == "the human clicked Deny");
}

BOOST_AUTO_TEST_CASE(a_denial_without_words_still_refuses)
{
    eventbus::AsyncEventBus bus;
    AnsweringConfirmer confirmer{ConfirmDecision::Denied, ""};
    auto subscription = bus.subscribe<InvokeConfirmEvent>(answering(confirmer));

    const auto [passed, reason] =
        check_on(settled_query(model_io::InvokeSecurity::RequireConfirm), bus);

    BOOST_TEST(!passed);
    BOOST_TEST(reason == "the confirmation handler denied the invocation");
}

BOOST_AUTO_TEST_CASE(a_handler_that_does_not_answer_refuses)
{
    eventbus::AsyncEventBus bus;
    // A handler that only observes: it returns the request unchanged, so the
    // decision stays Unanswered and silence refuses.
    bool observed = false;
    auto subscription = bus.subscribe<InvokeConfirmEvent>(
        [&observed](const InvokeConfirmEvent& request)
            -> asio::awaitable<InvokeConfirmEvent> {
            observed = true;
            co_return request;
        });

    const auto [passed, reason] =
        check_on(settled_query(model_io::InvokeSecurity::RequireConfirm), bus);

    BOOST_TEST(observed);
    BOOST_TEST(!passed);
    BOOST_TEST(reason ==
               "no subscribed handler answered the confirmation request");
}

BOOST_AUTO_TEST_CASE(the_last_handler_to_answer_decides)
{
    eventbus::AsyncEventBus bus;
    AnsweringConfirmer ui{ConfirmDecision::Approved, "the human said yes"};

    // Registration order is fold order, so the policy layer receives the UI's
    // approval and is the last word.
    ConfirmDecision policy_saw = ConfirmDecision::Unanswered;
    auto first = bus.subscribe<InvokeConfirmEvent>(answering(ui));
    auto second = bus.subscribe<InvokeConfirmEvent>(
        [&policy_saw](const InvokeConfirmEvent& request)
            -> asio::awaitable<InvokeConfirmEvent> {
            policy_saw = request.decision;
            InvokeConfirmEvent out = request;
            out.decision = ConfirmDecision::Denied;
            out.reason = "the policy forbids it";
            co_return out;
        });

    const auto [passed, reason] =
        check_on(settled_query(model_io::InvokeSecurity::RequireConfirm), bus);

    BOOST_TEST(ui.asked);
    BOOST_CHECK(policy_saw == ConfirmDecision::Approved);
    BOOST_TEST(!passed);
    BOOST_TEST(reason == "the policy forbids it");
}

BOOST_AUTO_TEST_CASE(a_throwing_handler_refuses_by_propagating)
{
    eventbus::AsyncEventBus bus;
    // A confirmer that fails instead of answering. Throwing from a coroutine
    // surfaces at the await, inside the bus's fold — which is why the throw is
    // conditional on a real query rather than dangling after it: this has to be
    // a coroutine, and it has to throw where a handler runs.
    auto subscription = bus.subscribe<InvokeConfirmEvent>(
        [](const InvokeConfirmEvent& request)
            -> asio::awaitable<InvokeConfirmEvent> {
            if (request.query.name == "write_file") {
                throw std::runtime_error("the confirmation dialog crashed");
            }
            co_return request;
        });

    // A broken confirmer is not silence: its exception surfaces at the caller,
    // where the toolset reports it at Stage::SecurityCheck. Either way the
    // invocation does not run.
    try {
        check_on(settled_query(model_io::InvokeSecurity::RequireConfirm), bus);
        BOOST_FAIL("the handler's exception should have propagated");
    } catch (const std::runtime_error& e) {
        BOOST_TEST(std::string(e.what()) == "the confirmation dialog crashed");
    }
}

BOOST_AUTO_TEST_CASE(a_throwing_handler_does_not_stop_a_later_one)
{
    // Propagate mode, for better or worse: the fold aborts on the throw, so a
    // handler registered after the broken one never runs.
    eventbus::AsyncEventBus bus;
    bool later_ran = false;

    auto broken = bus.subscribe<InvokeConfirmEvent>(
        [](const InvokeConfirmEvent& request)
            -> asio::awaitable<InvokeConfirmEvent> {
            if (request.query.name == "write_file") {
                throw std::runtime_error("the confirmation service is unreachable");
            }
            co_return request;
        });
    auto later = bus.subscribe<InvokeConfirmEvent>(
        [&later_ran](const InvokeConfirmEvent& request)
            -> asio::awaitable<InvokeConfirmEvent> {
            later_ran = true;
            InvokeConfirmEvent out = request;
            out.decision = ConfirmDecision::Approved;
            co_return out;
        });

    BOOST_CHECK_THROW(
        check_on(settled_query(model_io::InvokeSecurity::RequireConfirm), bus),
        std::runtime_error);
    BOOST_TEST(!later_ran);
}

BOOST_AUTO_TEST_CASE(the_default_bus_is_the_process_wide_one)
{
    eventbus::AsyncEventBus& bus = eventbus::default_async_bus();
    BOOST_REQUIRE(bus.subscriber_count<InvokeConfirmEvent>() == 0u);

    const auto [passed, reason] = check_on_default_bus(
        settled_query(model_io::InvokeSecurity::RequireConfirm));

    // Nobody is on the process-wide bus, so a RequireConfirm call is refused:
    // that bus is the default argument, and it is empty here.
    BOOST_TEST(!passed);
    BOOST_TEST(reason == "no handler is subscribed to confirm the invocation");

    // A host subscribed there — the front end of a plugin, say — is what turns
    // the same call into a run. ScopedSubscription, so the process-wide bus is
    // left as it was found even if an assertion inside the scope fails.
    {
        AnsweringConfirmer confirmer{ConfirmDecision::Approved, "confirmed by the host"};
        eventbus::AsyncEventBus::ScopedSubscription subscription =
            bus.subscribe<InvokeConfirmEvent>(answering(confirmer));

        const auto [host_passed, host_reason] = check_on_default_bus(
            settled_query(model_io::InvokeSecurity::RequireConfirm));

        BOOST_TEST(host_passed);
        BOOST_TEST(host_reason == "confirmed by the host");
        BOOST_TEST(confirmer.asked);
    }

    BOOST_TEST(bus.subscriber_count<InvokeConfirmEvent>() == 0u);
}
