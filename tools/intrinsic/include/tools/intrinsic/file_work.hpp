#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <system_error>
#include <type_traits>
#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>

namespace tools::intrinsic {

/**
 * Bounded blocking-file executor shared by a worker's reading/editing tools.
 *
 * One thread runs at most one scan at a time; at most max_pending invocations
 * may be active or queued. Excess calls fail instead of growing an unbounded
 * background queue. A caller awaiting run() always joins its work, even if its
 * Asio cancellation slot fires: cooperative cancellation uses the captured
 * std::stop_token and must preserve publication outcomes.
 *
 * The optional provider is called on the invoking executor, before enqueueing,
 * to snapshot the current run's token. It must be quick and safe on that
 * executor. Queued work owns that token, never observes a later run's token,
 * and owns all arguments it uses. Keep the application executor running until
 * calls finish; stopping io_context is not a substitute for joined shutdown.
 */
class FileWork : public std::enable_shared_from_this<FileWork> {
public:
    using StopProvider = std::function<std::stop_token()>;

    explicit FileWork(StopProvider stop = {}, std::size_t max_pending = 32)
        : stop_(std::move(stop)), max_pending_(max_pending)
    {
        if (max_pending == 0) throw std::invalid_argument("file work capacity must be positive");
    }

    /** Joins the owned thread; normal shutdown has already awaited all jobs. */
    ~FileWork() { pool_.join(); }

    /**
     * Invoke an owning callable on the blocking thread and return on the
     * caller's executor. Errors, including pre-publication cancellation, reach
     * the ordinary tool-result failure path. No cancellation is tested after
     * work returns: a published edit must not be hidden by a late stop. An
     * empty child cancellation slot shields only this job; it does not reset
     * the caller's cancellation state or change later await semantics.
     */
    template <class Function>
    boost::asio::awaitable<std::invoke_result_t<Function, std::stop_token>> run(Function work)
    {
        // Acquire ownership eagerly: awaitables are lazy, and a host may drop
        // its tool/runtime handle before the scheduled coroutine starts.
        return run_owned(shared_from_this(), std::move(work));
    }

private:
    template <class Function>
    static boost::asio::awaitable<std::invoke_result_t<Function, std::stop_token>> run_owned(
        std::shared_ptr<FileWork> owner, Function work)
    {
        using Result = std::invoke_result_t<Function, std::stop_token>;
        const auto stop = owner->stop_ ? owner->stop_() : std::stop_token{};
        if (owner->pending_.fetch_add(1) >= owner->max_pending_) {
            owner->pending_.fetch_sub(1);
            throw std::runtime_error("file I/O queue is full; retry after pending operations finish");
        }
        struct Reservation {
            std::atomic<std::size_t>& count;
            ~Reservation() { count.fetch_sub(1); }
        } reservation{owner->pending_};
        co_return co_await boost::asio::co_spawn(owner->pool_,
            [work = std::move(work), stop]() mutable -> boost::asio::awaitable<Result> {
                if (stop.stop_requested()) {
                    throw std::system_error(std::make_error_code(std::errc::operation_canceled),
                                            "file operation cancelled before execution");
                }
                co_return work(stop);
            }, boost::asio::bind_cancellation_slot(
                boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
    }

    StopProvider stop_;
    const std::size_t max_pending_;
    std::atomic<std::size_t> pending_{0};
    boost::asio::thread_pool pool_{1};
};

} // namespace tools::intrinsic
