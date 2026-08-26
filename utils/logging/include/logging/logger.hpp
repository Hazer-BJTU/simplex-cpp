#pragma once

/**
 * @file logger.hpp
 * @brief Thread-safe formatted console logger for the simplex-cpp project.
 *
 * Provides a simple Logger class with severity levels (trace, debug, info,
 * warning, error, fatal).  INFO / DEBUG / WARNING go to stdout; ERROR / FATAL
 * go to stderr.  A static std::mutex serialises all output so the logger is
 * safe to call from any thread — including boost::asio coroutines that may
 * execute on a thread pool.
 *
 * Every log call uses std::format (C++20) and prepends a UTC timestamp and
 * the severity label, e.g.:
 *
 *   [2026-07-16 14:31:02.123] [INFO ] Listening on port 8080
 *   [2026-07-16 14:31:02.456] [ERROR] Connection refused
 *
 * Usage:
 *   #include "logging/logger.hpp"
 *   Logger::info("Client connected from {}", addr);
 *   Logger::error("Failed to open '{}': {}", path, ec.message());
 */

#include <chrono>
#include <format>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>

namespace logging {

// =============================================================================
// LogLevel enum
// =============================================================================

/**
 * @brief Severity levels for log messages.
 *
 * The numeric values follow the usual syslog-inspired ordering so that
 * comparisons like `level >= LogLevel::warning` work as expected.
 */
enum class LogLevel {
    trace   = 0,
    debug   = 1,
    info    = 2,
    warning = 3,
    error   = 4,
    fatal   = 5,
};

/** @brief Stream insertion for log levels (useful in test assertions). */
inline std::ostream& operator<<(std::ostream& os, LogLevel lvl) {
    constexpr const char* names[] = {
        "trace", "debug", "info", "warning", "error", "fatal"
    };
    return os << names[static_cast<int>(lvl)];
}

// =============================================================================
// Compile-time debug switch
// =============================================================================

/**
 * @brief Compile-time constant: true in Debug builds, false otherwise.
 *
 * `LOGGING_DEBUG` is treated as a plain on/off flag: defined (by any means —
 * `-DLOGGING_DEBUG`, `-DLOGGING_DEBUG=1`, or the CMake Debug config) means true;
 * absent means false.  This avoids the pitfalls of inspecting the macro's
 * value, which would break for a bare `-DLOGGING_DEBUG` (an empty expansion).
 *
 * Drives the `if constexpr (kDebugBuild)` gate inside the verbose level
 * methods (trace/debug).  When false those method bodies are discarded at
 * compile time, so the formatting and I/O never run.  (Argument expressions at
 * the call site are still evaluated as usual for an inline call — keep debug
 * arguments cheap, or hoist expensive work into the call only when it matters.)
 */
inline constexpr bool kDebugBuild =
#ifdef LOGGING_DEBUG
    true;
#else
    false;
#endif

// =============================================================================
// Logger
// =============================================================================

/**
 * @brief A minimal, thread-safe console logger.
 *
 * Every public method locks an internal static mutex, formats the message
 * with a UTC timestamp and severity label, and writes the result atomically
 * to the appropriate standard stream.
 *
 * The class is entirely static — no instantiation is needed.  An optional
 * global minimum level filters out messages below the configured threshold
 * (default: LogLevel::info in release builds, LogLevel::debug in debug
 * builds).
 */
class Logger {
public:
    Logger()  = delete;
    ~Logger() = delete;

    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------

    /**
     * @brief Set the global minimum log level.
     *
     * Messages whose level is **strictly less** than @p min_level are
     * silently dropped.  The default is LogLevel::info in release builds
     * and LogLevel::debug in debug builds (selected at compile time via
     * @ref kDebugBuild).
     */
    static void set_level(LogLevel min_level) noexcept;

    /** @brief Return the current global minimum log level. */
    static LogLevel level() noexcept;

    // -------------------------------------------------------------------------
    // Formatted logging (variadic, std::format)
    // -------------------------------------------------------------------------
    //
    // The verbose levels — trace and debug — are compile-time gated by
    // kDebugBuild: in non-debug builds their bodies are discarded, so the
    // std::format call and the underlying I/O never happen.  info and above
    // are always available (runtime level-filtered only).  Call sites can thus
    // write `Logger::debug(...)` directly with no wrapper and pay nothing in
    // release builds.

    /** @brief Log a TRACE-level message (debug-build only). */
    template <typename... Args>
    static void trace(std::format_string<Args...> fmt, Args&&... args) {
        if constexpr (kDebugBuild) {
            log(LogLevel::trace, std::format(fmt, std::forward<Args>(args)...));
        }
    }

    /** @brief Log a DEBUG-level message (debug-build only). */
    template <typename... Args>
    static void debug(std::format_string<Args...> fmt, Args&&... args) {
        if constexpr (kDebugBuild) {
            log(LogLevel::debug, std::format(fmt, std::forward<Args>(args)...));
        }
    }

    /** @brief Log an INFO-level message. */
    template <typename... Args>
    static void info(std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::info, std::format(fmt, std::forward<Args>(args)...));
    }

    /** @brief Log a WARNING-level message. */
    template <typename... Args>
    static void warning(std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::warning, std::format(fmt, std::forward<Args>(args)...));
    }

    /** @brief Log an ERROR-level message (writes to stderr). */
    template <typename... Args>
    static void error(std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::error, std::format(fmt, std::forward<Args>(args)...));
    }

    /** @brief Log a FATAL-level message (writes to stderr). */
    template <typename... Args>
    static void fatal(std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::fatal, std::format(fmt, std::forward<Args>(args)...));
    }

    // -------------------------------------------------------------------------
    // Pre-formatted string logging (when the caller already has a string)
    // -------------------------------------------------------------------------

    static void trace(std::string_view msg) { if constexpr (kDebugBuild) { log(LogLevel::trace, msg); } }
    static void debug(std::string_view msg) { if constexpr (kDebugBuild) { log(LogLevel::debug, msg); } }
    static void info(std::string_view msg)  { log(LogLevel::info,  msg); }
    static void warning(std::string_view msg){ log(LogLevel::warning, msg); }
    static void error(std::string_view msg)  { log(LogLevel::error, msg); }
    static void fatal(std::string_view msg)  { log(LogLevel::fatal, msg); }

    // -------------------------------------------------------------------------
    // Core: write a pre-formatted message at a given level
    // -------------------------------------------------------------------------

    /**
     * @brief Low-level log entry point.
     *
     * If @p lvl >= the configured minimum level, the message is formatted
     * with a UTC timestamp and severity label, then written (with a trailing
     * newline) to stdout (for TRACE…WARNING) or stderr (ERROR, FATAL).
     *
     * The internal mutex is held for the duration of the write so that
     * multi-line log output from concurrent callers never interleaves.
     */
    static void log(LogLevel lvl, std::string_view message);

private:
    static std::mutex    _mutex;
    static LogLevel      _min_level;

    /** Render the UTC timestamp prefix string (e.g. "[2026-07-16 14:31:02.123]"). */
    static std::string _timestamp();
};

} // namespace logging
