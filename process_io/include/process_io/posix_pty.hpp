#pragma once

#include <memory>

#include "process_io/environment.hpp"

namespace process_io {

/** Linux PTY with merged raw output and independently cancellable input.
 * Uses the host strand and owns no child. Launch with prepare_attachment(),
 * then start(); stop/reap the child separately and always cancel_and_join().
 * Queued EOF checks current terminal settings when it reaches the writer;
 * unsupported mode becomes InputPhase::Unsupported, without closing output.
 * Ended means the EOF request was written, not a physical half-close. Programs
 * can change termios or ignore EOF. Closing the master at the final fence can
 * hang up slave holders, including descendants; it is not a process-tree kill.
 * Host defaults are 80x24, canonical/signals enabled, echo disabled, TERM=dumb.
 * resize() is host-only. No escape stripping or terminal emulation is provided.
 */
class PosixPtyIo final : public ProcessIoEnvironment {
public:
    explicit PosixPtyIo(Executor executor, std::size_t max_output_bytes = 4 * 1024 * 1024);
    ~PosixPtyIo() override;
    PosixPtyIo(const PosixPtyIo&) = delete;
    PosixPtyIo& operator=(const PosixPtyIo&) = delete;

    [[nodiscard]] Executor executor() const override;
    [[nodiscard]] Capabilities capabilities() const noexcept override;
    subprocess::IoAttachment prepare_attachment() override;
    boost::asio::awaitable<void> start() override;
    boost::asio::awaitable<InputResult> enqueue_input(std::string input) override;
    boost::asio::awaitable<InputResult> request_input_end() override;
    boost::asio::awaitable<void> stop_admission() override;
    boost::asio::awaitable<void> cancel_input() override;
    [[nodiscard]] Snapshot snapshot() const override;
    [[nodiscard]] const CaptureBuffer& capture() const noexcept override;
    boost::asio::awaitable<bool> drain_until(std::chrono::steady_clock::time_point deadline) override;
    boost::asio::awaitable<void> cancel_and_join() override;
    boost::asio::awaitable<ResizeResult> resize(unsigned columns, unsigned rows) override;

private:
    struct State;
    std::shared_ptr<State> _state;
};

} // namespace process_io
