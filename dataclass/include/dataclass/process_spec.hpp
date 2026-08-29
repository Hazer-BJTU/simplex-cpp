#pragma once

//
// process_spec.hpp — the subprocess data contract
// ==============================================
//
// The data half of the process domain, the way model_io.hpp is the data half
// of the model domain: plain aggregates describing what to launch (and the
// environment and timeout policy to launch it under), how the execution is
// going, and what came out. The process/ module's manager (its framework is
// being written against these types) consumes and produces them; anything
// async, pipes or platform behaviour lives there, not here.
//
// Relationship to the legacy indextools shapes: the wire words reuse the old
// execution_status() vocabulary ("running" | "exited" | "unknown"), but the
// display-side shapes (schema.hpp's meta tables, its null-as-absent stream
// fields, the report builder) are deliberately NOT reproduced — those are
// presentation contracts of a consumer that no longer builds. This header is
// the typed data contract that replaces them.
//
// Same contract rules as model_io.hpp / endpoint_config.hpp: plain aggregates
// serialised through nlohmann ADL (a to_json/from_json pair next to each
// struct), snake_case keys, optionals omitted when empty and never null,
// enums as lowercase snake_case strings with the fail-safe value listed first
// (unknown values fall back to it on read), unknown keys ignored, find()
// guards instead of at(), round-trip invariant json(x).get<X>() reproduces x.
//

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/types.h>

#include <nlohmann/json.hpp>

namespace process_io {

// ---- state ----------------------------------------------------------------

// Where a spawned process is in its lifecycle. Unknown is listed first so an
// unrecognised state string on read fails safe: the report says "I don't
// know" instead of guessing "running". The words are the legacy
// execution_status() vocabulary minus "finished": running = alive under the
// manager, exited = the OS-level terminal state (exit_code observable),
// unknown = the state cannot be determined.
enum class ProcessState {
    Unknown,
    Running,
    Exited,
};

NLOHMANN_JSON_SERIALIZE_ENUM(ProcessState, {
    {ProcessState::Unknown, "unknown"},
    {ProcessState::Running, "running"},
    {ProcessState::Exited, "exited"},
})

// ---- launch request -------------------------------------------------------

// What to launch and how to babysit it — the typed form of a spawn request
// (the legacy manager took description/executable/args as loose parameters;
// timeout, env and post-timeout behaviour were call-site conventions).
struct LaunchSpec {
    // Executable name (resolved through PATH) or a path. Resolution failures
    // surface as process::ProcessException at Stage::ResolveExecutable.
    std::string executable;
    // argv tail — argv[0] is the executable itself, not repeated here.
    // Passed through verbatim: there is no shell on the other side, so
    // operators and redirections reach the child as literal arguments.
    std::vector<std::string> arguments;
    // Human-readable label carried through to every report about this
    // process; the manager never parses it.
    std::string description;
    // The Linux pid of the process this spec refers to — literally pid_t,
    // not a manager-assigned handle. Absent in a pure launch request; the
    // manager engages it once the process exists, so a result echoing its
    // spec carries the pid set.
    std::optional<pid_t> pid;
    // Required on every launch — always present on the wire, unlike the
    // optional fields. The deadline this launch waits against; there is no
    // optional fire-and-forget form anymore.
    std::uint64_t timeout_milliseconds = 0;
    // Only meaningful at the deadline: true (default) detaches and leaves
    // the child running, false kills it. The negation of the legacy
    // kill_on_timeout switch.
    bool detach_on_timeout = true;
    // Additional environment entries for the child, applied on top of the
    // inherited environment when inherit_environment is true, or as the
    // whole environment when it is false. Disengaged => no explicit entries.
    std::optional<std::unordered_map<std::string, std::string>> environment;
    // true (default) => the child inherits the parent's environment.
    bool inherit_environment = true;
};

inline void to_json(nlohmann::json& j, const LaunchSpec& s) {
    j = nlohmann::json{
        {"executable", s.executable},
        {"arguments", s.arguments},
        {"description", s.description},
        {"timeout_milliseconds", s.timeout_milliseconds},
        {"detach_on_timeout", s.detach_on_timeout},
        {"inherit_environment", s.inherit_environment},
    };
    if (s.pid) j["pid"] = *s.pid;
    if (s.environment) j["environment"] = *s.environment;
}

inline void from_json(const nlohmann::json& j, LaunchSpec& s) {
    if (auto it = j.find("executable"); it != j.end()) it->get_to(s.executable);
    if (auto it = j.find("arguments"); it != j.end()) it->get_to(s.arguments);
    if (auto it = j.find("description"); it != j.end()) it->get_to(s.description);
    if (auto it = j.find("timeout_milliseconds"); it != j.end())
        it->get_to(s.timeout_milliseconds);
    if (auto it = j.find("detach_on_timeout"); it != j.end())
        it->get_to(s.detach_on_timeout);
    if (auto it = j.find("inherit_environment"); it != j.end())
        it->get_to(s.inherit_environment);
    // JSON null reads as ABSENT — see the never-null round-trip rule in the
    // header comment (same guard detail::read_optional applies in model_io).
    if (auto it = j.find("pid"); it != j.end() && !it->is_null())
        s.pid = it->get<pid_t>();
    else s.pid.reset();
    if (auto it = j.find("environment"); it != j.end() && !it->is_null())
        s.environment =
            it->get<std::unordered_map<std::string, std::string>>();
    else s.environment.reset();
}

// ---- execution status -----------------------------------------------------

// The typed form of the legacy execution_status() record. exit_code engages
// only once the process has exited; the contract here omits it when empty
// (the legacy JSON wrote null — a presentation choice this contract drops).
// The duration field is named cumulative because it keeps accumulating over
// the process's lifetime — every status read reports the running total, not a
// one-shot final measurement.
struct ExecutionStatus {
    ProcessState state = ProcessState::Unknown;
    std::optional<int> exit_code;
    std::uint64_t cumulative_execution_milliseconds = 0;
};

inline void to_json(nlohmann::json& j, const ExecutionStatus& e) {
    j = nlohmann::json{
        {"state", e.state},
        {"cumulative_execution_milliseconds",
         e.cumulative_execution_milliseconds},
    };
    if (e.exit_code) j["exit_code"] = *e.exit_code;
}

inline void from_json(const nlohmann::json& j, ExecutionStatus& e) {
    if (auto it = j.find("state"); it != j.end()) it->get_to(e.state);
    if (auto it = j.find("cumulative_execution_milliseconds"); it != j.end())
        it->get_to(e.cumulative_execution_milliseconds);
    if (auto it = j.find("exit_code"); it != j.end() && !it->is_null())
        e.exit_code = it->get<int>();
    else e.exit_code.reset();
}

// ---- result ----------------------------------------------------------------

// One process as reported to a caller: the launch spec that started it,
// echoed verbatim with the pid engaged (spec + pid is the process's
// identity — the old manager-assigned id and duplicated description are
// gone), how the execution went, and whichever output streams were captured.
// The stream fields carry a _text suffix because bare stdout/stderr collide
// with the <cstdio> macros of the same name; the wire keys keep the suffix
// for the usual keys-match-fields rule. Disengaged stream fields mean "not
// captured for this report" — a meta-only status listing, say — never
// "empty output".
struct ExecutionResult {
    LaunchSpec spec;
    ExecutionStatus execution;
    std::optional<std::string> stdout_text;
    std::optional<std::string> stderr_text;
};

inline void to_json(nlohmann::json& j, const ExecutionResult& r) {
    j = nlohmann::json{
        {"spec", r.spec},
        {"execution", r.execution},
    };
    if (r.stdout_text) j["stdout_text"] = *r.stdout_text;
    if (r.stderr_text) j["stderr_text"] = *r.stderr_text;
}

inline void from_json(const nlohmann::json& j, ExecutionResult& r) {
    if (auto it = j.find("spec"); it != j.end()) it->get_to(r.spec);
    if (auto it = j.find("execution"); it != j.end()) it->get_to(r.execution);
    if (auto it = j.find("stdout_text"); it != j.end() && !it->is_null())
        r.stdout_text = it->get<std::string>();
    else r.stdout_text.reset();
    if (auto it = j.find("stderr_text"); it != j.end() && !it->is_null())
        r.stderr_text = it->get<std::string>();
    else r.stderr_text.reset();
}

} // namespace process_io
