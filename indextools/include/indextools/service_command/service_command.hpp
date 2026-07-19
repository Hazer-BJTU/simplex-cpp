#pragma once

/**
 * @file service_command.hpp
 * @brief Abstract base class for the service-command pattern.
 *
 * Every command the server can execute (search, edit, subprocess inspection,
 * etc.) inherits from ServiceCommand and implements three pure-virtual methods:
 * name(), ensure_params(), and execute().  The non-virtual operator() is the
 * single entry point — it validates input, runs the command, and catches
 * exceptions, centralising the error-handling policy so individual commands
 * never need to duplicate it.
 *
 * ## Lifecycle of a command invocation
 *
 *     params ──► ensure_params() ──► execute() ──► result (JSON)
 *                    │   ▲                │   ▲
 *                    ▼   │                ▼   │
 *                 error[]  │            error[] │
 *                          │                    │
 *                (caught exception)    (caught exception)
 *
 * 1. `ensure_params(params, error)` validates and normalises the raw request.
 *    - On success: return the normalised params; leave `error` alone.
 *    - On failure: push errors into `error` (an array); the return value is ignored.
 * 2. If `error` is non-empty after step 1, the error array is returned immediately.
 * 3. `execute(normalised_params, error)` runs the actual work.
 *    - Same contract: push into `error` on failure, return the result on success.
 * 4. If an exception escapes either step, it is caught and pushed as a
 *    structured ErrorReport (schema.hpp) into the `error` array, whose
 *    `command` field is `name()`.
 *
 * ## Implementing a new command
 *
 * @code
 * class MyCommand : public ServiceCommand {
 * public:
 *     std::string_view name() const noexcept override { return "my_command"; }
 *
 *     nlohmann::json ensure_params(nlohmann::json params,
 *                                   nlohmann::json& error) const override {
 *         if (!params.contains("key")) {
 *             error = schema::validation_error(name(), schema::error_code::invalid_argument,
 *                                              "Missing required parameter 'key'").build();
 *             return {};
 *         }
 *         return params;
 *     }
 *
 *     boost::asio::awaitable<nlohmann::json> execute(nlohmann::json params,
 *                                                     nlohmann::json& error) override {
 *         // ... do the work ...
 *         nlohmann::json result;
 *         result["status"] = "ok";
 *         co_return result;
 *     }
 * };
 * @endcode
 */

#include <format>
#include <string>
#include <string_view>

#include "logging/logger.hpp"
#include "indextools/schema.hpp"

#include <boost/asio.hpp>

#include <nlohmann/json.hpp>

#if defined(DEBUG_BUILD)
inline constexpr bool PRINT_CMD_DEBUG_INFO = true;
#else
inline constexpr bool PRINT_CMD_DEBUG_INFO = false;
#endif

namespace indextools {

// Logger lives in the sibling `logging` module (namespace `logging`). Pull the
// class name into this namespace so call sites can write `Logger::error(...)`.
using logging::Logger;

/**
 * @brief Abstract base class for all service commands.
 *
 * Subclasses implement the three pure-virtual methods.  Callers invoke the
 * command through operator(), which handles validation, execution, exception
 * translation, and optional debug logging in one place.
 */
class ServiceCommand {
public:
    virtual ~ServiceCommand() = 0;

    /**
     * @brief Human-readable command name.
     *
     * Used as the `Command` field in error reports.  Must be a compile-time
     * constant or a stable string — the return value is a view, not an owned
     * string, so it must outlive the command object.
     */
    virtual std::string_view name() const noexcept = 0;

    /**
     * @brief Validate and normalise raw request parameters.
     *
     * @param params  Raw parameters from the incoming request (moved in).
     * @param error   Output parameter.  On validation failure, populate this
     *                with a structured error (see schema::validation_error)
     *                and return any value.  Leave untouched on success.
     * @return        Normalised parameters on success; ignored on failure.
     *
     * @note The default implementation in operator() moves @p params, so this
     *       receives an rvalue.  Implementations that only inspect can accept
     *       by const-ref; the signature uses by-value for flexibility.
     */
    virtual nlohmann::json ensure_params(nlohmann::json params,
                                         nlohmann::json& error) const = 0;

    /**
     * @brief Execute the command with validated parameters.
     *
     * @param ensured_params  Normalised parameters from ensure_params().
     * @param error           Output parameter.  On execution failure, populate
     *                        this with a structured error (see
     *                        schema::execution_error) and return any value.
     *                        Leave untouched on success.
     * @return                The command result on success; ignored on failure.
     */
    virtual boost::asio::awaitable<nlohmann::json> execute(
        nlohmann::json ensured_params, nlohmann::json& error) = 0;

    /**
     * @brief Entry point: validate → execute → return result or error.
     *
     * Orchestrates the full command lifecycle:
     * 1. Calls ensure_params().  If it populates `error` or throws, the error
     *    is returned and execute() is never called.
     * 2. Calls execute().  If it populates `error` or throws, the error is
     *    returned.
     * 3. Otherwise, the result from execute() is returned to the caller.
     *
     * In debug builds (PRINT_CMD_DEBUG_INFO), every error path also logs the
     * command name and the full error JSON via Logger::error().
     *
     * @param params  Raw request parameters (moved in).
     * @return        A JSON value — either the command result or a structured
     *                error report (see schema::ErrorReport).
     */
    boost::asio::awaitable<nlohmann::json> operator()(nlohmann::json params) noexcept {
        nlohmann::json ensured_params, result, error = nlohmann::json::array();
        try {
            ensured_params = ensure_params(std::move(params), error);
        } catch (const std::exception& e) {
            error.push_back(schema::validation_error(
                name(),
                schema::error_code::invalid_argument,
                std::string("Exception: ") + e.what()
            ).build());
        }

        if (!error.empty()) {
            if constexpr (PRINT_CMD_DEBUG_INFO) {
                Logger::error("ensure_params failed for command {}: {}", name(), error.dump());
            }
            co_return error;
        }

        try {
            result = co_await execute(std::move(ensured_params), error);
        } catch (const std::exception& e) {
            error.push_back(schema::execution_error(
                name(),
                schema::error_code::internal_error,
                std::string("Exception: ") + e.what()
            ).build());
        }

        if (!error.empty()) {
            if constexpr (PRINT_CMD_DEBUG_INFO) {
                Logger::error("execute failed for command {}: {}", name(), error.dump());
            }
            co_return error;
        }
        co_return result;
    }

    template<typename T>
    T expect_field(const nlohmann::json& obj, std::string_view field_name, nlohmann::json& error) const noexcept {
        if (!obj.contains(field_name)) {
            auto msg = schema::validation_error(
                name(),
                schema::error_code::invalid_argument,
                std::format("missing argument: {}", field_name)
            ).build();

            if (error.is_array()) {
                error.push_back(std::move(msg));
            } else {
                error = std::move(msg);
            }

            return {};
        }

        try {
            auto field = obj.at(field_name).get<T>();
            return field;
        } catch(const std::exception& e) {
            auto msg = schema::validation_error(
                name(),
                schema::error_code::invalid_argument,
                std::format("argument type error for '{}': {}", field_name, e.what())
            ).build();

            if (error.is_array()) {
                error.push_back(std::move(msg));
            } else {
                error = std::move(msg);
            }

            return {};
        }
    }

    template<typename T>
    T expect_field_or(const nlohmann::json& obj, std::string_view field_name, const T& default_value) const noexcept {
        nlohmann::json error;
        auto result = expect_field<T>(obj, field_name, error);
        return error.empty() ? result : default_value;
    }

    void condition(bool expression, std::string message, nlohmann::json& error) const noexcept {
        if (expression) {
            return;
        }

        auto msg = schema::validation_error(
            name(),
            schema::error_code::invalid_argument,
            std::move(message)
        ).build();

        if (error.is_array()) {
            error.push_back(msg);
        } else {
            error = std::move(msg);
        }
        return;
    }
};

inline ServiceCommand::~ServiceCommand() {}

} // namespace indextools
