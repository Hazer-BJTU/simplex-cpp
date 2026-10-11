#pragma once

#include "process_io/anonymous_pipe.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace asio = boost::asio;
using namespace std::chrono_literals;

/** Every test cleans up even when an assertion/coroutine throws. The child
 * remains separate from the environment: only this host fixture kills/reaps it.
 */
struct Harness {
    asio::io_context context;
    process_io::Executor strand{asio::make_strand(context)};
    std::unique_ptr<process_io::AnonymousPipeIo> environment;
    std::unique_ptr<subprocess::Child> child;

    explicit Harness(std::size_t cap = 4 * 1024 * 1024)
        : environment(std::make_unique<process_io::AnonymousPipeIo>(strand, cap))
    {}

    void launch(const std::string& mode)
    {
        const auto attachment = environment->prepare_attachment();
        process::LaunchSpec spec;
        spec.executable = PROCESS_IO_CHILD;
        spec.arguments = {mode};
        child = std::make_unique<subprocess::Child>(spec, strand, attachment);
    }

    asio::awaitable<void> pause(std::chrono::milliseconds duration = 5ms)
    {
        asio::steady_timer timer(strand);
        timer.expires_after(duration);
        co_await timer.async_wait(asio::use_awaitable);
    }

    asio::awaitable<int> wait_exit()
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        boost::system::error_code error;
        while (child->running(error)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("fixture child did not exit");
            }
            co_await pause();
        }
        if (error) throw boost::system::system_error(error);
        co_return child->exit_code();
    }

    asio::awaitable<void> ready()
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (environment->snapshot().standard_output->bytes != "ready") {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("fixture child did not become ready");
            }
            co_await pause();
        }
    }

    void run(std::function<asio::awaitable<void>(Harness&)> test, std::size_t workers = 1)
    {
        auto done = asio::co_spawn(strand,
            [this, test = std::move(test)]() -> asio::awaitable<void> {
                std::exception_ptr failure;
                try {
                    co_await test(*this);
                } catch (...) {
                    failure = std::current_exception();
                }
                try {
                    if (environment) co_await environment->cancel_and_join();
                } catch (...) {
                    if (!failure) failure = std::current_exception();
                }
                if (failure) std::rethrow_exception(failure);
            }, asio::use_future);
        std::vector<std::jthread> threads;
        for (std::size_t i = 1; i < workers; ++i) {
            threads.emplace_back([this] { context.run(); });
        }
        context.run();
        threads.clear();
        // The native emergency fallback requires a quiescent context and
        // joined threads, even though this fixture never starts a child watcher.
        std::exception_ptr failure;
        try {
            done.get();
        } catch (...) {
            failure = std::current_exception();
        }
        if (child) {
            const auto error = child->emergency_cleanup();
            if (error && !failure) {
                failure = std::make_exception_ptr(boost::system::system_error(error));
            }
        }
        if (failure) std::rethrow_exception(failure);
    }
};
