#include "persistence/store.hpp"

#include <ctime>
#include <string>
#include <utility>

namespace persistence {

const char* backend_name(Backend backend) {
    switch (backend) {
        case Backend::Json: return "json";
    }
    throw std::logic_error("invalid persistence backend");
}

const char* boundary_name(Boundary boundary) {
    switch (boundary) {
        case Boundary::BeforeTools: return "before_tools";
        case Boundary::ResultsReady: return "results_ready";
        case Boundary::StepFinished: return "step_finished";
        case Boundary::RunFinished: return "run_finished";
        case Boundary::Cancelled: return "cancelled";
        case Boundary::Shutdown: return "shutdown";
        case Boundary::Compaction: return "compact";
    }
    throw std::logic_error("invalid persistence boundary");
}

StoreError::StoreError(
    const std::string& message, Backend backend, CommitStatus status,
    std::string operation_id)
    : std::runtime_error(message), backend_(backend), status_(status),
      operation_id_(std::move(operation_id)) {}

StoreError::~StoreError() = default;

StateStore::~StateStore() = default;

std::string utc_timestamp() {
    const auto now = std::time(nullptr);
    std::tm utc{};
    char text[32]{};
    if (!::gmtime_r(&now, &utc)
        || std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        throw std::runtime_error("cannot format session timestamp");
    }
    return text;
}

} // namespace persistence
