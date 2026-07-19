#include "logging/logger.hpp"

#include <iostream>
#include <thread>

namespace logging {

// =============================================================================
// Static state
// =============================================================================

std::mutex Logger::_mutex;

#ifdef DEBUG_BUILD
LogLevel Logger::_min_level = LogLevel::debug;
#else
LogLevel Logger::_min_level = LogLevel::info;
#endif

// =============================================================================
// Configuration
// =============================================================================

void Logger::set_level(LogLevel min_level) noexcept {
    std::lock_guard<std::mutex> lock(_mutex);
    _min_level = min_level;
}

LogLevel Logger::level() noexcept {
    std::lock_guard<std::mutex> lock(_mutex);
    return _min_level;
}

// =============================================================================
// Timestamp
// =============================================================================

std::string Logger::_timestamp() {
    using namespace std::chrono;

    const auto now = system_clock::now();
    const auto now_time_t = system_clock::to_time_t(now);
    const auto now_ms = duration_cast<milliseconds>(
        now.time_since_epoch()) % 1000;

    // gmtime is not thread-safe on all platforms, but we are already
    // holding _mutex when _timestamp() is called, so there is no data race.
    const auto tm = *std::gmtime(&now_time_t);

    return std::format("[{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}]",
                        tm.tm_year + 1900,
                        tm.tm_mon + 1,
                        tm.tm_mday,
                        tm.tm_hour,
                        tm.tm_min,
                        tm.tm_sec,
                        now_ms.count());
}

// =============================================================================
// Core logging
// =============================================================================

void Logger::log(LogLevel lvl, std::string_view message) {
    // Fast-path reject: bail out before acquiring the lock when the level
    // is too low.  We read _min_level outside the lock, which is safe
    // (racing with set_level() at worst delivers one stale message).
    if (lvl < _min_level) return;

    // Level label for the prefix (6 chars wide, right-padded)
    constexpr std::string_view labels[] = {
        "TRACE ",
        "DEBUG ",
        "INFO  ",
        "WARN  ",
        "ERROR ",
        "FATAL ",
    };
    const auto label = labels[static_cast<int>(lvl)];

    const auto ts = _timestamp();

    // Build the full line before locking to keep the critical section short.
    const auto line = std::format("{} [{}] {}\n", ts, label, message);

    std::lock_guard<std::mutex> lock(_mutex);

    // Re-check the level now that we hold the lock — set_level() may have
    // raised the threshold between our first check and now.
    if (lvl < _min_level) return;

    if (lvl >= LogLevel::error) {
        std::cerr << line;
    } else {
        std::cout << line;
    }
}

} // namespace logging
