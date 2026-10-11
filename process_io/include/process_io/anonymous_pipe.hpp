#pragma once

#include <memory>

#include "process_io/environment.hpp"

namespace process_io {

/** Three anonymous pipes with one input queue and one authoritative capture
 * store. Accepts a supplied strand, never wraps it in another strand. Pump
 * state has shared ownership until all completion handlers settle; destroying
 * an unjoined facade requests cancellation on its strand as a defensive tail.
 * That tail still needs a running context, or eventual context destruction;
 * callers needing completion MUST await cancel_and_join(). No child is owned
 * or signalled here. The host remains responsible for launch and reaping.
 */
class AnonymousPipeIo final : public ProcessIoEnvironment {
public:
    explicit AnonymousPipeIo(Executor executor, std::size_t max_output_bytes = 4 * 1024 * 1024);
    ~AnonymousPipeIo() override;
    AnonymousPipeIo(const AnonymousPipeIo&) = delete;
    AnonymousPipeIo& operator=(const AnonymousPipeIo&) = delete;

    [[nodiscard]] Executor executor() const override;
    [[nodiscard]] Capabilities capabilities() const noexcept override;
    subprocess::IoAttachment prepare_attachment() override;
    boost::asio::awaitable<void> start() override;
    boost::asio::awaitable<InputResult> enqueue_input(std::string input) override;
    boost::asio::awaitable<InputResult> request_input_end() override;
    boost::asio::awaitable<void> stop_admission() override;
    [[nodiscard]] Snapshot snapshot() const override;
    boost::asio::awaitable<bool> drain_until(std::chrono::steady_clock::time_point deadline) override;
    boost::asio::awaitable<void> cancel_and_join() override;
    boost::asio::awaitable<ResizeResult> resize(unsigned columns, unsigned rows) override;

private:
    struct State;
    std::shared_ptr<State> _state;
};

} // namespace process_io
