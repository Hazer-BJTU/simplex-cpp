#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>
#include "fileio/session_lock.hpp"

namespace manager {

/** A safe public diagnostic. Messages never include configuration contents. */
class Error : public std::runtime_error {
public:
    Error(std::string code, std::string message);
    ~Error() override;
    const std::string code;
};

/**
 * Execute one version-1 management request synchronously on the target host.
 * Returns a complete JSON result or throws Error/system errors. No receive or
 * snapshot byte limit is imposed. Callers must keep this blocking API off an
 * IO executor. Only explicitly owned allocations can be changed or removed.
 * clean_fork addresses the child and names a source allocation/lifecycle/revision;
 * it derives configuration under the source's shared lease and publishes only
 * fresh child storage. Retries reuse the captured source, never runtime state.
 */
nlohmann::json execute(const nlohmann::json& request);

/**
 * Effective configuration and shared allocation lease for a managed worker.
 * Retain the lease through the entire worker lifetime, including shutdown.
 * An exclusive mutation of this allocation cannot run while its lease is alive.
 * clean_fork may read it as a source without modifying it.
 */
struct Configuration {
    nlohmann::json document;
    std::shared_ptr<fileio::SessionLock> lease;
};

/**
 * Recognize a manager-generated configuration reference. Ordinary YAML returns
 * nullopt. Managed references are resolved under a shared allocation lock and
 * must match their original target path and allocation identity. Never create
 * missing storage or silently fall back to an unmanaged configuration.
 */
std::optional<Configuration> resolve_configuration(
    const std::filesystem::path& file, const nlohmann::json& document);

} // namespace manager
