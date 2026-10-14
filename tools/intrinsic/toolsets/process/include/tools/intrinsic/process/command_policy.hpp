#pragma once

#include <memory>
#include <string_view>

#include <nlohmann/json.hpp>

namespace tools::intrinsic {

/** Startup-only approval policy for run_command, independent of process launch.
 *
 * Regexes classify complete, unexpanded simple commands. A conservative shell
 * scanner rejects syntax it cannot safely partition before any matching occurs.
 * This is an operator-controlled heuristic, not a sandbox: executable lookup,
 * environment, configuration and filesystem contents can change program effects.
 * Instances own immutable compiled rules and can be queried concurrently.
 */
class CommandPolicy {
public:
    /// Readonly changes scheduling as well as approval; trusted writes stay serial.
    enum class Decision { Confirm, Readonly, TrustedWrite };

    /// Empty configuration means strict. Throws invalid_argument on malformed
    /// configuration, with a field/index diagnostic but without command/rule data.
    explicit CommandPolicy(const nlohmann::json& config);

    /// Return Confirm on unsupported syntax, size/complexity limits or matching
    /// failures. Does not execute, expand, normalize or modify the supplied command.
    [[nodiscard]] Decision classify(std::string_view command) const noexcept;

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};

} // namespace tools::intrinsic
