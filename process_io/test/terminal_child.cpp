#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t resized = 0;
// The handler only sets signal-safe state; formatting and output stay in main().
void on_resize(int)
{
    resized = 1;
}

/// Use native writes so readiness markers and unterminated text are not hidden
/// in iostream buffering. Retry EINTR without duplicating already written bytes.
void output(const std::string& bytes)
{
    const char* cursor = bytes.data();
    std::size_t left = bytes.size();
    while (left) {
        const auto n = ::write(1, cursor, left);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            ::_exit(10);
        }
        cursor += n;
        left -= n;
    }
}
}
// Standalone terminal fixture: modes probe launch state, change EOF handling,
// stress capture, or retain a slave in a descendant for bounded-shutdown tests.
int main(int argc, char** argv)
{
    if (argc != 2) {
        return 2;
    }
    const std::string mode = argv[1];
    termios settings{};
    if (::tcgetattr(0, &settings) < 0) {
        return 3;
    }
    if (mode.starts_with("fd:")) {
        const int fd = std::stoi(mode.substr(3));
        const bool closed = ::fcntl(fd, F_GETFD) < 0 && errno == EBADF;
        output(closed ? "fd-closed" : "fd-leaked");
        return closed ? 0 : 9;
    }
    if (mode == "hold-slave") {
        ::signal(SIGHUP, SIG_IGN);
        const pid_t child = ::fork();
        if (child < 0) {
            return 10;
        }
        if (child == 0) {
            ::sleep(30);
            return 0;
        }
        output("descendant:" + std::to_string(child) + "\n");
        return 0;
    }
    if (mode == "probe" || mode == "probe-clean") {
        if (mode == "probe-clean" && ::getenv("SIMPLEX_PTY_PARENT_ONLY")) {
            return 11;
        }
        winsize size{};
        if (::ioctl(0, TIOCGWINSZ, &size)) {
            return 4;
        }
        const int tty = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
        const bool ok = ::isatty(0) && ::isatty(1) && ::isatty(2) && tty >= 0
                        && ::getsid(0) == ::getpid() && ::tcgetpgrp(0) == ::getpgrp()
                        && (settings.c_lflag & ICANON) && (settings.c_lflag & ISIG)
                        && !(settings.c_lflag & (ECHO | ECHONL)) && size.ws_col == 80
                        && size.ws_row == 24;
        if (tty >= 0) {
            ::close(tty);
        }
        output(ok ? "probe-ok:" : "probe-failed:");
        output(::getenv("TERM") ? ::getenv("TERM") : "missing");
        return ok ? 0 : 5;
    }
    if (mode == "raw") {
        ::cfmakeraw(&settings);
    }
    if (mode == "disabled-eof") {
        settings.c_cc[VEOF] = _POSIX_VDISABLE;
    }
    if (mode == "signal-eof") {
        settings.c_cc[VEOF] = settings.c_cc[VINTR];
    }
    if (mode == "custom-eof") {
        settings.c_cc[VEOF] = 5;
    }
    if (::tcsetattr(0, TCSANOW, &settings)) {
        return 6;
    }
    if (mode == "resize") {
        struct sigaction action{};
        action.sa_handler = on_resize;
        ::sigemptyset(&action.sa_mask);
        ::sigaction(SIGWINCH, &action, nullptr);
    }
    output("ready"); // Deliberately no newline: prompts must be captured promptly.
    if (mode == "no-read") {
        ::sleep(30);
        return 0;
    }
    if (mode == "burst") {
        const std::string bytes(4096, 'x');
        for (int i = 0; i < 128; ++i) {
            output(bytes);
            if (::write(2, bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size())) {
                return 7;
            }
        }
        return 0;
    }
    if (mode == "resize") {
        // Block SIGWINCH during observation and use sigsuspend to avoid the
        // check/pause race. A signal arriving before this point is retained.
        sigset_t set, old;
        ::sigemptyset(&set);
        ::sigaddset(&set, SIGWINCH);
        ::sigprocmask(SIG_BLOCK, &set, &old);
        while (!resized) {
            ::sigsuspend(&old);
        }
        winsize size{};
        ::ioctl(0, TIOCGWINSZ, &size);
        output(std::to_string(size.ws_col) + "x" + std::to_string(size.ws_row));
        return 0;
    }
    std::array<char, 4096> buffer;
    for (;;) {
        const auto count = ::read(0, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return 8;
        }
        if (count == 0) {
            break;
        }
        output(std::string(buffer.data(), count));
    }
    output("out-tail\n");
    const std::string error = "err-tail\n";
    ::write(2, error.data(), error.size());
}
