#pragma once

/**
 * @file shell.hpp
 * @brief Shell command support for the service-command framework.
 *
 * Provides subprocess execution as a ServiceCommand:
 *   - SpawnWaitCommand wraps SubProcessManager::spawn_and_wait() with
 *     parameter validation, timeout parsing, and structured errors.
 *   - The free function parse_duration_to_seconds() converts human-readable
 *     duration strings (e.g. "30s", "5m", "2h30m") to a size_t in seconds.
 *
 * # Duration string format
 *
 * A duration string is a sequence of number+unit pairs, plus an optional
 * trailing bare number (interpreted as seconds). Supported units (case-
 * insensitive):
 *
 *   S — seconds   (multiplier:       1)
 *   M — minutes   (multiplier:      60)
 *   H — hours     (multiplier:    3600)
 *   D — days      (multiplier:   86400)
 *   W — weeks     (multiplier:  604800)
 *
 * Examples:
 *   "30s"      →    30
 *   "5m"       →   300
 *   "2h30m"    →  9000
 *   "1h30m45s" →  5445
 *   "s"        →     1  (implicit "1" when number is omitted)
 *   "5m30"     →   330  (trailing bare number = seconds)
 *   "0s"       →     0
 *
 * Allowed characters: digits (0-9), letters (s/m/h/d/w, case-insensitive).
 * Any other character (spaces, punctuation, etc.) throws std::invalid_argument.
 */

#include "indextools/service_command/service_command.hpp"
#include "indextools/subprocess.hpp"

#include <cctype>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace indextools {

// =============================================================================
// parse_duration_to_seconds
// =============================================================================

/**
 * @brief Parse a human-readable duration string into seconds.
 *
 * Accepts a compact format of number+unit pairs:
 *   - Numbers are unsigned integers.  Leading zeros are allowed.
 *   - Units are single letters: S, M, H, D, W (case-insensitive).
 *   - A number without a following unit is treated as a bare-seconds suffix
 *     (e.g. "5m30" → 5 minutes + 30 seconds).
 *   - A unit without a preceding number defaults to 1 (e.g. "s" → 1 second).
 *
 * @param str  The duration string to parse (e.g. "30s", "2h30m").
 * @return     The total duration in seconds as a size_t.
 *
 * @throws std::invalid_argument  if the string is empty, contains an
 *                                unrecognised character, or uses an invalid
 *                                unit letter.
 * @throws std::overflow_error    if any intermediate value or the final sum
 *                                exceeds SIZE_MAX.
 */
size_t parse_duration_to_seconds(std::string_view str) {
    if (str.empty()) {
        throw std::invalid_argument("empty duration string");
    }

    size_t total_seconds = 0;
    size_t current_value = 0;
    bool has_number = false;

    for (size_t i = 0; i < str.length(); ++i) {
        // Cast to unsigned char: std::isdigit / std::isalpha / std::toupper
        // have undefined behaviour when called with a plain char whose value
        // is negative (non-ASCII byte).
        const auto uc = static_cast<unsigned char>(str[i]);

        // ---- digit ----
        if (std::isdigit(uc)) {
            size_t digit = uc - '0';

            // Check for overflow BEFORE multiplying by 10.
            if (current_value > (SIZE_MAX - digit) / 10) {
                throw std::overflow_error("Duration value overflow");
            }
            current_value = current_value * 10 + digit;
            has_number = true;
            continue;
        }

        // ---- unit letter ----
        if (std::isalpha(uc)) {
            // Implicit "1": a unit letter without a preceding number,
            // e.g. "s" → 1 second, "m" → 1 minute.
            if (!has_number) {
                current_value = 1;
            }

            const char upper_c = static_cast<char>(std::toupper(uc));
            size_t multiplier = 0;

            switch (upper_c) {
                case 'S': multiplier = 1;      break;
                case 'M': multiplier = 60;     break;
                case 'H': multiplier = 3600;   break;
                case 'D': multiplier = 86400;  break;
                case 'W': multiplier = 604800; break;
                default:
                    throw std::invalid_argument(
                        std::string("invalid time unit: '") + static_cast<char>(uc) + "'"
                    );
            }

            // Check overflow of current_value * multiplier.
            if (current_value > SIZE_MAX / multiplier) {
                throw std::overflow_error("Duration value overflow");
            }

            const size_t increment = current_value * multiplier;

            // Check overflow of total_seconds + increment (cumulative sum).
            if (total_seconds > SIZE_MAX - increment) {
                throw std::overflow_error("Duration value overflow");
            }

            total_seconds += increment;
            current_value = 0;
            has_number = false;
            continue;
        }

        // ---- invalid character ----
        throw std::invalid_argument(
            std::string("invalid character in duration: '") + static_cast<char>(uc) + "'"
        );
    }

    // Trailing bare number treated as seconds (e.g. "5m30" → 5 min + 30 s).
    if (has_number && current_value > 0) {
        // Check overflow on the final addition.
        if (total_seconds > SIZE_MAX - current_value) {
            throw std::overflow_error("Duration value overflow");
        }
        total_seconds += current_value;
    }

    return total_seconds;
}

// =============================================================================
// SpawnWaitCommand
// =============================================================================

/**
 * @brief Service command that spawns a child process and waits for it.
 *
 * Wraps SubProcessManager::spawn_and_wait() behind the ServiceCommand
 * interface so shell execution can be dispatched through the standard
 * command pipeline (validate → execute → structured result/error).
 *
 * # Parameters (raw JSON → validated by ensure_params)
 *
 *   - exec_name  (string, required)       Executable name (resolved via PATH).
 *   - arguments  (string array, required) Command-line arguments.
 *   - timeout    (string, default "3s")   Per-step timeout in duration format
 *                                         (e.g. "30s", "5m", "1h").
 *   - detach_after_timeout (bool, default true)
 *       true  → timeout detaches the child (keeps running); the result
 *               reflects a partial (still running) snapshot.
 *       false → timeout kills the child (SIGTERM) and reaps it; the result
 *               carries the final exit code.
 *   - description (string, auto-generated) Human-readable label built from
 *                                         exec_name + arguments.
 *
 * # Result (on success)
 *
 *   A schema::ProcessReport JSON object with meta (ID, Description, Status,
 *   Exit Code, Elapsed ms) and stdout / stderr streams.
 *
 * # Errors
 *
 *   Validation errors (missing exec_name, invalid argument type, bad timeout
 *   format) are pushed to the error array during ensure_params().
 *
 *   Execution errors (subprocess spawn failure, timeout, etc.) are pushed
 *   during execute().
 */
class SpawnWaitCommand : public ServiceCommand {
private:
    std::shared_ptr<SubProcessManager> _manager;

public:
    explicit SpawnWaitCommand(std::shared_ptr<SubProcessManager> manager)
        : _manager(std::move(manager)) {}

    ~SpawnWaitCommand() = default;
    SpawnWaitCommand(const SpawnWaitCommand&) = default;
    SpawnWaitCommand& operator=(const SpawnWaitCommand&) = default;
    SpawnWaitCommand(SpawnWaitCommand&&) = default;
    SpawnWaitCommand& operator=(SpawnWaitCommand&&) = default;

    /** @brief Command identifier used in error reports and dispatch tables. */
    std::string_view name() const noexcept override {
        return "shell.spawn_and_wait";
    }

    /**
     * @brief Validate and normalise the raw request parameters.
     *
     * Extracts exec_name (required), arguments (required), timeout (optional,
     * default "3s"), detach_after_timeout (optional, default true), and
     * description (optional, auto-generated from exec_name + args).
     *
     * The timeout string is parsed into timeout_seconds (size_t) via
     * parse_duration_to_seconds().
     *
     * @param params  Raw request JSON.
     * @param error   Output — populated with structured validation errors on
     *                failure.  Left untouched on success.
     * @return        Normalised params on success; ignored on failure (error
     *                is checked by the caller).
     */
    nlohmann::json ensure_params(nlohmann::json params, nlohmann::json& error) const override {
        nlohmann::json ensured_params;

        // ---- required: exec_name ----
        std::string exec_name = expect_field<std::string>(params, "exec_name", error);

        // ---- optional with defaults ----
        std::string timeout = expect_field_or<std::string>(params, "timeout", "3s");
        bool detach_after_timeout = expect_field_or<bool>(params, "detach_after_timeout", true);

        // ---- required: arguments (must be a string array) ----
        std::vector<std::string> arguments;
        try {
            auto args_field = params["arguments"];

            if (!args_field.is_array()) {
                throw std::runtime_error(
                    std::format("argument list must be an array, got: {}", args_field.dump()));
            }

            for (auto argument : args_field) {
                arguments.push_back(argument.get<std::string>());
            }
        } catch (const std::exception& e) {
            error.push_back(schema::validation_error(
                name(),
                schema::error_code::invalid_argument,
                std::format("invalid argument list: {}", e.what())
            ).build());
        }

        // Bail out early if any validation failed — avoids calling
        // parse_duration_to_seconds on a potentially invalid timeout string.
        if (!error.empty()) {
            return {};
        }

        // ---- auto-generated description ----
        std::ostringstream default_description;
        default_description << exec_name;
        for (const auto& argument : arguments) {
            default_description << " " << argument;
        }

        std::string description = expect_field_or<std::string>(
            params, "description", default_description.str());

        // Assemble the normalised parameter set.
        ensured_params = {
            {"exec_name", exec_name},
            {"timeout_seconds", parse_duration_to_seconds(timeout)},
            {"detach_after_timeout", detach_after_timeout},
            {"description", description}
        };
        ensured_params["arguments"] = std::move(arguments);

        return ensured_params;
    }

    /**
     * @brief Execute the command: spawn a child process and wait for it.
     *
     * Delegates to SubProcessManager::spawn_and_wait().  The timeout
     * behaviour is controlled by detach_after_timeout:
     *   - true  → detach on timeout (kill=false), process keeps running.
     *   - false → kill on timeout (kill=true), process is terminated.
     *
     * @param ensured_params  Normalised params from ensure_params().
     * @param error           Output — populated with a structured execution
     *                        error on failure.  Left untouched on success.
     * @return                A ProcessReport JSON on success; empty object on
     *                        error.
     */
    boost::asio::awaitable<nlohmann::json> execute(
        nlohmann::json ensured_params, nlohmann::json& error) override
    {
        std::string exec_name = ensured_params["exec_name"];
        std::vector<std::string> arguments = ensured_params["arguments"];
        std::string description = ensured_params["description"];
        size_t timeout_seconds = ensured_params["timeout_seconds"];
        bool detach_after_timeout = ensured_params["detach_after_timeout"];

        nlohmann::json response;
        try {
            response = co_await _manager->spawn_and_wait(
                description,
                exec_name,
                std::move(arguments),
                std::chrono::seconds(timeout_seconds),
                !detach_after_timeout   // kill = NOT detach
            );
        } catch (const std::exception& e) {
            error.push_back(schema::execution_error(
                name(),
                schema::error_code::subprocess,
                std::format("execution error: {} of command: {}, operation aborted",
                            e.what(), description)
            ).build());
        }

        co_return (error.empty() ? response : nlohmann::json{});
    }
};

} // namespace indextools
