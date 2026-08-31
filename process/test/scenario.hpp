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

// Spawns the spec, runs the standard lifecycle (start tasks -> optional
// mid-life callback while the child is guaranteed alive -> await against the
// deadline) and waits for full quiescence. The callback is where a case
// feeds stdin or observes the live (Running) view.
inline Scenario run_scenario(
    process::LaunchSpec spec,
    std::function<void(process::ProcessHandle&)> on_running = {})
{
    Scenario s;
    s.io = std::make_unique<boost::asio::io_context>();
    auto strand = boost::asio::make_strand(*s.io);

    s.before_spawn = std::chrono::system_clock::now();
    s.handle = std::make_shared<process::ProcessHandle>(std::move(spec), strand);
    s.after_spawn = std::chrono::system_clock::now();

    auto done = boost::asio::co_spawn(
        strand,
        [handle = s.handle, &finished = s.finished_on_time, on_running]()
            -> boost::asio::awaitable<void> {
            co_await handle->start_background_io_tasks();
            if (on_running) on_running(*handle);
            finished = co_await handle->await_initial_execution();
        },
        boost::asio::use_future);

    s.io->run();
    done.get();
    return s;
}

} // namespace process_test
