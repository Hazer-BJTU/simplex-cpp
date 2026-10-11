#define BOOST_TEST_MODULE ProcessIoPtyFailures
#include <boost/test/unit_test.hpp>
#include "harness.hpp"
#include "process_io/posix_pty.hpp"
#include <atomic>
#include <cerrno>
#include <dlfcn.h>
#include <filesystem>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
enum class Fault { None, Allocate, Grant, Session, Dup, Read, Write };
std::atomic<Fault> fault{Fault::None};
std::atomic<bool> observed{false};
// Resolve symbols before any fork; no dlsym, C++ static guard or allocation is
// introduced by the child-side interposition paths.
const auto real_openpt = reinterpret_cast<int(*)(int)>(::dlsym(RTLD_NEXT, "posix_openpt"));
const auto real_grantpt = reinterpret_cast<int(*)(int)>(::dlsym(RTLD_NEXT, "grantpt"));
const auto real_setsid = reinterpret_cast<pid_t(*)()>(::dlsym(RTLD_NEXT, "setsid"));
const auto real_dup2 = reinterpret_cast<int(*)(int,int)>(::dlsym(RTLD_NEXT, "dup2"));
const auto real_read = reinterpret_cast<ssize_t(*)(int,void*,size_t)>(::dlsym(RTLD_NEXT, "read"));
const auto real_write = reinterpret_cast<ssize_t(*)(int,const void*,size_t)>(::dlsym(RTLD_NEXT, "write"));
bool consume(Fault expected)
{
    if (!fault.compare_exchange_strong(expected, Fault::None)) return false;
    observed = true;
    errno = EIO;
    return true;
}
bool master(int fd)
{
    int number;
    const int saved = errno;
    const bool result = ::ioctl(fd, TIOCGPTN, &number) == 0;
    errno = saved;
    return result;
}
std::size_t descriptors()
{
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                         std::filesystem::directory_iterator{});
}
}
extern "C" int posix_openpt(int flags) { return consume(Fault::Allocate) ? -1 : real_openpt(flags); }
extern "C" int grantpt(int fd) noexcept { return consume(Fault::Grant) ? -1 : real_grantpt(fd); }
extern "C" pid_t setsid() noexcept { return consume(Fault::Session) ? -1 : real_setsid(); }
extern "C" int dup2(int oldfd, int newfd) noexcept { return consume(Fault::Dup) ? -1 : real_dup2(oldfd, newfd); }
extern "C" ssize_t read(int fd, void* data, size_t size)
{
    if (fault == Fault::Read && master(fd) && consume(Fault::Read)) return -1;
    return real_read(fd, data, size);
}
extern "C" ssize_t write(int fd, const void* data, size_t size)
{
    if (fault == Fault::Write && master(fd) && consume(Fault::Write)) return -1;
    return real_write(fd, data, size);
}

BOOST_AUTO_TEST_CASE(allocation_and_child_setup_failures_roll_back)
{
    const auto before = descriptors();
    for (const auto point : {Fault::Allocate, Fault::Grant, Fault::Session, Fault::Dup}) {
        for (int iteration = 0; iteration < 8; ++iteration) {
            asio::io_context context;
            auto strand = asio::make_strand(context);
            observed = false;
            fault = point;
            bool caught = false;
            try {
                process_io::PosixPtyIo terminal(strand);
                auto attachment = terminal.prepare_attachment();
                process::LaunchSpec spec;
                spec.executable = PROCESS_PTY_CHILD;
                spec.arguments = {"probe"};
                subprocess::Child child(spec, strand, attachment);
            } catch (const subprocess::ProcessException& error) {
                caught = true;
                BOOST_TEST(error.error_code().value() == EIO);
                BOOST_TEST(std::string(error.what()).find(point == Fault::Session ? "PTY setsid" : "PTY dup2") != std::string::npos);
            } catch (const boost::system::system_error& error) {
                caught = true;
                BOOST_TEST(error.code().value() == EIO);
                BOOST_TEST(observed.load());
            }
            fault = Fault::None;
            BOOST_TEST(caught);
            context.run(); // unstarted facade's defensive tail
        }
    }
    BOOST_TEST(descriptors() == before);
    int status;
    BOOST_TEST(::waitpid(-1, &status, WNOHANG) == -1);
    BOOST_TEST(errno == ECHILD);
}
BOOST_AUTO_TEST_CASE(live_slave_eio_is_failure_and_join_still_completes)
{
    for (const auto point : {Fault::Read, Fault::Write}) {
        Harness h;
        h.environment = std::make_unique<process_io::PosixPtyIo>(h.strand);
        auto attachment = h.environment->prepare_attachment();
        process::LaunchSpec spec;
        spec.executable = PROCESS_PTY_CHILD;
        spec.arguments = {"no-read"};
        h.child = std::make_unique<subprocess::Child>(spec, h.strand, attachment);
        bool failed = false;
        try {
            h.run([point](Harness& h) -> asio::awaitable<void> {
                observed = false;
                fault = point;
                co_await h.environment->start();
                if (point == Fault::Write) co_await h.environment->enqueue_input("input\n");
                const auto deadline = std::chrono::steady_clock::now() + 2s;
                while (!h.environment->snapshot().failure && std::chrono::steady_clock::now() < deadline) {
                    co_await h.pause();
                }
                BOOST_TEST(observed.load());
                BOOST_TEST(static_cast<bool>(h.environment->snapshot().failure));
                if (point == Fault::Read) {
                    BOOST_CHECK(h.environment->snapshot().terminal_output->end == process_io::StreamEnd::Failed);
                } else {
                    BOOST_CHECK(h.environment->snapshot().input_phase == process_io::InputPhase::Failed);
                    BOOST_TEST(!h.environment->snapshot().output_complete);
                }
                try { co_await h.environment->cancel_and_join(); }
                catch (...) {
                    BOOST_TEST(h.environment->snapshot().joined);
                    throw;
                }
            });
        } catch (const boost::system::system_error& error) {
            failed = true;
            BOOST_TEST(error.code().value() == EIO);
        }
        fault = Fault::None;
        BOOST_TEST(failed);
    }
}
