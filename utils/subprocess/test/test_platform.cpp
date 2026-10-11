#define BOOST_TEST_MODULE subprocess_platform
#include <boost/test/unit_test.hpp>

#include "subprocess/child.hpp"
#include "subprocess/platform.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <type_traits>

static_assert(std::is_same_v<process::ProcessId, std::uint64_t>);

BOOST_AUTO_TEST_CASE(executable_location_identifies_host_not_shared_library)
{
    std::error_code error = std::make_error_code(std::errc::io_error);
    const auto executable = subprocess::executable_location(error);
    BOOST_REQUIRE(!error);
    BOOST_TEST(executable.is_absolute());
    BOOST_TEST(executable.filename().string() == "test_subprocess_platform");
    BOOST_TEST(subprocess::executable_location() == executable);
#if defined(__linux__)
    BOOST_TEST(std::filesystem::equivalent(
        executable, std::filesystem::read_symlink("/proc/self/exe")));
#endif
}

BOOST_AUTO_TEST_CASE(shell_uses_conventional_absolute_path_and_argument_prefix)
{
    const auto shell = subprocess::platform_shell();
    std::string expected = "/bin/sh";
    for (const auto* candidate : {"/bin/bash", "/usr/bin/bash"}) {
        std::error_code ignored;
        if (std::filesystem::is_regular_file(candidate, ignored)) {
            expected = candidate;
            break;
        }
    }
    BOOST_TEST(shell.executable == expected);
    BOOST_REQUIRE_EQUAL(shell.argument_prefix.size(), 1U);
    BOOST_TEST(shell.argument_prefix.front() == "-c");
}

BOOST_AUTO_TEST_CASE(owned_child_emergency_cleanup_reaps_and_is_repeatable)
{
    boost::asio::io_context context;
    boost::asio::writable_pipe input(context);
    boost::asio::readable_pipe output(context), error(context);
    const auto attachment = subprocess::IoAttachment::pipes(input, output, error);
    process::LaunchSpec spec;
    spec.executable = "sleep";
    spec.arguments = {"60"};
    subprocess::Child child(spec, context.get_executor(), attachment);
    BOOST_TEST(child.id() > 0U);
    boost::system::error_code failure;
    BOOST_REQUIRE(child.running(failure));
    BOOST_REQUIRE(!failure);
    BOOST_TEST(!child.emergency_cleanup());
    BOOST_TEST(!child.running(failure));
    BOOST_REQUIRE(!failure);
    BOOST_TEST(child.exit_code() == 9);
    BOOST_TEST(!child.emergency_cleanup());
}

BOOST_AUTO_TEST_CASE(moved_attachment_is_rejected_before_launch)
{
    boost::asio::io_context context;
    boost::asio::writable_pipe input(context);
    boost::asio::readable_pipe output(context), error(context);
    auto attachment = subprocess::IoAttachment::pipes(input, output, error);
    auto owner = std::move(attachment);
    process::LaunchSpec spec;
    spec.executable = "sleep";
    spec.arguments = {"60"};
    BOOST_CHECK_EXCEPTION(
        subprocess::Child(spec, context.get_executor(), attachment),
        subprocess::ProcessException,
        [](const auto& failure) {
            return failure.stage() == subprocess::ProcessException::Stage::Spawn;
        });
}

BOOST_AUTO_TEST_CASE(retaining_attachment_does_not_hold_child_pipe_ends_open)
{
    boost::asio::io_context context;
    boost::asio::writable_pipe input(context);
    boost::asio::readable_pipe output(context), error(context);
    // Keep the attachment alive through EOF. The native child-side bindings
    // must have been released at the end of construction, not at this scope's end.
    const auto attachment = subprocess::IoAttachment::pipes(input, output, error);
    process::LaunchSpec spec;
    spec.executable = "echo";
    spec.arguments = {"tail"};
    subprocess::Child child(spec, context.get_executor(), attachment);
    std::string captured;
    boost::system::error_code read_error;
    auto read = boost::asio::co_spawn(context,
        [&]() -> boost::asio::awaitable<void> {
            co_await boost::asio::async_read(output,
                boost::asio::dynamic_buffer(captured),
                boost::asio::redirect_error(boost::asio::use_awaitable, read_error));
        }, boost::asio::use_future);
    context.run();
    read.get();
    BOOST_TEST(read_error == boost::asio::error::eof);
    BOOST_TEST(captured == "tail\n");
    BOOST_TEST(!child.emergency_cleanup());
}
