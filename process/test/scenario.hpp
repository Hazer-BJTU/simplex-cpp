#pragma once

//
// scenario.hpp — shared driver for the ProcessHandle tests
// =========================================================
//
// One child per scenario, run on a private io_context + strand (the usage
// pattern the class comment documents) to full quiescence before any
// assertion runs, so what assertions see is the settled aftermath: final
// status recorded, pipes drained, stdin channel closed.
//
// Scenario owns the io_context BEFORE the handle on purpose — declaration
// order is destruction order, so the handle (and its asio objects) is
// always destroyed before its io_context.
//

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <boost/asio/use_future.hpp>

#include "process/process_handle.hpp"

namespace process_test {

struct Scenario {
    std::unique_ptr<boost::asio::io_context> io;
    std::shared_ptr<process::ProcessHandle> handle;
    bool finished_on_time = false;
    std::chrono::system_clock::time_point before_spawn;
    std::chrono::system_clock::time_point after_spawn;
};

// The mid-life hook, in its two shapes. Sync is what most cases want (feed
// stdin, observe the live view); async is for a case whose mid-life action is
// itself an awaitable — terminate(), request_exit() — which a std::function
// returning void cannot express. Both run ON THE STRAND, between the io tasks
// starting and the deadline wait, where the child is guaranteed alive.
using SyncHook = std::function<void(process::ProcessHandle&)>;
using AsyncHook =
    std::function<boost::asio::awaitable<void>(process::ProcessHandle&)>;

// Spawns the spec, runs the standard lifecycle (start tasks -> optional
// mid-life hooks while the child is guaranteed alive -> await against the
// deadline) and waits for full quiescence.
inline Scenario run_scenario(
    process::LaunchSpec spec,
    SyncHook on_running = {},
    AsyncHook on_running_async = {})
{
    Scenario s;
    s.io = std::make_unique<boost::asio::io_context>();
    auto strand = boost::asio::make_strand(*s.io);

    s.before_spawn = std::chrono::system_clock::now();
    s.handle = std::make_shared<process::ProcessHandle>(std::move(spec), strand);
    s.after_spawn = std::chrono::system_clock::now();

    auto done = boost::asio::co_spawn(
        strand,
        [handle = s.handle, &finished = s.finished_on_time, on_running,
         on_running_async]() -> boost::asio::awaitable<void> {
            co_await handle->start_background_io_tasks();
            if (on_running) on_running(*handle);
            if (on_running_async) co_await on_running_async(*handle);
            finished = co_await handle->await_initial_execution();
        },
        boost::asio::use_future);

    s.io->run();
    done.get();
    return s;
}

} // namespace process_test
