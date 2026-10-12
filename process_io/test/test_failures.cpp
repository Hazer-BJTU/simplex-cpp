#define BOOST_TEST_MODULE ProcessIoFailures
#include <boost/test/unit_test.hpp>
#include "harness.hpp"

#include <atomic>
#include <cerrno>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
std::atomic<int> fail_read{0};
std::atomic<int> fail_write{0};

/// Fail one owned pipe operation, leaving stdio/reactor descriptors unaffected
/// and allowing all later cleanup operations to use the real native functions.
bool inject(int fd, std::atomic<int>& remaining)
{
    if (fd <= 2 || remaining.load() == 0) {
        return false;
    }
    struct stat status{};
    if (::fstat(fd, &status) != 0 || !S_ISFIFO(status.st_mode)) {
        return false;
    }
    int expected = 1;
    return remaining.compare_exchange_strong(expected, 0);
}
}

// Exercise real Asio pipe operations without exposing descriptors or test
// switches in the environment API. The child is a separately exec'd fixture.
extern "C" ssize_t read(int fd, void* buffer, size_t size)
{
    using Function = ssize_t (*)(int, void*, size_t);
    static auto original = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "read"));
    if (inject(fd, fail_read)) {
        errno = EIO;
        return -1;
    }
    return original(fd, buffer, size);
}

extern "C" ssize_t write(int fd, const void* buffer, size_t size)
{
    using Function = ssize_t (*)(int, const void*, size_t);
    static auto original = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "write"));
    if (inject(fd, fail_write)) {
        errno = EIO;
        return -1;
    }
    return original(fd, buffer, size);
}

using namespace process_io;

BOOST_AUTO_TEST_CASE(read_failure_is_visible_and_rethrown_only_after_all_pumps_join)
{
    Harness h;
    h.run([](Harness& h) -> asio::awaitable<void> {
        h.launch("echo");
        co_await h.environment->start();
        fail_read = 1;
        co_await h.environment->enqueue_input("trigger read");
        co_await h.environment->request_input_end();
        BOOST_TEST(co_await h.environment->drain_until(std::chrono::steady_clock::now() + 5s));
        BOOST_TEST(fail_read.load() == 0);
        auto snapshot = h.environment->snapshot();
        BOOST_REQUIRE(snapshot.failure);
        BOOST_TEST(!snapshot.joined);
        const auto failed = snapshot.standard_output->end == StreamEnd::Failed
                                ? snapshot.standard_output
                                : snapshot.standard_error;
        BOOST_CHECK(failed->end == StreamEnd::Failed);
        BOOST_TEST(failed->error.value() == EIO);
        BOOST_TEST(failed->truncated);
        for (int i = 0; i < 2; ++i) {
            bool reported = false;
            try {
                co_await h.environment->cancel_and_join();
            } catch (const boost::system::system_error& error) {
                reported = true;
                BOOST_TEST(error.code().value() == EIO);
            }
            BOOST_TEST(reported);
            BOOST_TEST(h.environment->snapshot().joined);
        }
        h.environment.reset(); // Joined; the expected retained error was checked.
    });
}

BOOST_AUTO_TEST_CASE(write_failure_stops_admission_and_does_not_abandon_readers)
{
    Harness h;
    h.run([](Harness& h) -> asio::awaitable<void> {
        h.launch("no-read");
        co_await h.environment->start();
        co_await h.ready();
        fail_write = 1;
        co_await h.environment->enqueue_input("trigger write");
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!h.environment->snapshot().failure) {
            BOOST_REQUIRE((std::chrono::steady_clock::now() < deadline));
            co_await h.pause();
        }
        BOOST_TEST(fail_write.load() == 0);
        BOOST_CHECK(h.environment->snapshot().input_phase == InputPhase::Failed);
        BOOST_CHECK((co_await h.environment->enqueue_input("late")) == InputResult::Closed);
        BOOST_TEST(!h.environment->snapshot().output_complete);
        bool reported = false;
        try {
            co_await h.environment->cancel_and_join();
        } catch (const boost::system::system_error& error) {
            reported = true;
            BOOST_TEST(error.code().value() == EIO);
        }
        BOOST_TEST(reported);
        BOOST_TEST(h.environment->snapshot().joined);
        BOOST_TEST(h.environment->snapshot().output_complete);
        BOOST_TEST(h.environment->snapshot().standard_output->bytes == "ready");
        h.environment.reset();
    });
}
