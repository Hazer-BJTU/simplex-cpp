#pragma once

//
// process_io.hpp — the subprocess data contract
// ==============================================
//
// The data half of the process domain, the way model_io.hpp is the data half
// of the model domain: plain aggregates describing what to launch, how the
// launch is going, and what came out. The process/ module's manager (its
// framework is being written against these types) consumes and produces them;
// anything async, pipes or platform behaviour lives there, not here.
//
// Relationship to the legacy indextools shapes: the wire words reuse the old
// execution_status() vocabulary ("running" | "finished" | "exited" |
// "unknown"), but the display-side shapes (schema.hpp's meta tables, its
// null-as-absent stream fields, the ProcessReport builder) are deliberately
// NOT reproduced — those are presentation contracts of a consumer that no
// longer builds. This header is the typed data contract that replaces them.
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
#include <vector>

#include <nlohmann/json.hpp>

namespace process_io {

// ---- state ----------------------------------------------------------------

// Where a spawned process is in its lifecycle. Unknown is listed first so an
// unrecognised state string on read fails safe: the report says "I don't
// know" instead of guessing "running". The words are the legacy
// execution_status() vocabulary: running = alive under the manager, finished
// = reaped after full output collection, exited = the OS-level terminal
// state, unknown = the handle no longer resolves to an instance.
enum class ProcessState {
    Unknown,
    Running,
    Finished,
    Exited,
};

NLOHMANN_JSON_SERIALIZE_ENUM(ProcessState, {
    {ProcessState::Unknown, "unknown"},
    {ProcessState::Running, "running"},
    {ProcessState::Finished, "finished"},
    {ProcessState::Exited, "exited"},
})

// ---- launch request -------------------------------------------------------

// What to launch and how to babysit it — the typed form of a spawn request
// (the legacy manager took description/executable/args plus, for the waiting
// flavour, a timeout and a kill switch as loose parameters).
struct ProcessSpec {
    // Human-readable label carried through to every report about this
    // process; the manager never parses it.
    std::string description;
    // Executable name (resolved through PATH) or a path. Resolution failures
    // surface as process::ProcessException at Stage::ResolveExecutable.
    std::string executable;
    // argv tail — argv[0] is the executable itself, not repeated here.
    std::vector<std::string> arguments;
    // Engaged => the launch waits for exit with this deadline; disengaged =>
    // fire-and-forget (the manager still reaps in the background).
    std::optional<std::uint64_t> timeout_milliseconds;
    // Only meaningful with an engaged timeout: true (default) kills the
    // process at the deadline, false detaches and leaves it running.
    bool kill_on_timeout = true;
    std::optional<nlohmann::json> extras;
};

inline void to_json(nlohmann::json& j, const ProcessSpec& s) {
    j = nlohmann::json{
        {"description", s.description},
        {"executable", s.executable},
        {"arguments", s.arguments},
        {"kill_on_timeout", s.kill_on_timeout},
    };
    if (s.timeout_milliseconds) j["timeout_milliseconds"] = *s.timeout_milliseconds;
    if (s.extras) j["extras"] = *s.extras;
}

inline void from_json(const nlohmann::json& j, ProcessSpec& s) {
    if (auto it = j.find("description"); it != j.end()) it->get_to(s.description);
    if (auto it = j.find("executable"); it != j.end()) it->get_to(s.executable);
    if (auto it = j.find("arguments"); it != j.end()) it->get_to(s.arguments);
    if (auto it = j.find("kill_on_timeout"); it != j.end())
        it->get_to(s.kill_on_timeout);
    if (auto it = j.find("timeout_milliseconds"); it != j.end() && !it->is_null())
        s.timeout_milliseconds = it->get<std::uint64_t>();
    else s.timeout_milliseconds.reset();
    // JSON null reads as ABSENT — see the never-null round-trip rule in the
    // header comment (same guard detail::read_optional applies in model_io).
    if (auto it = j.find("extras"); it != j.end() && !it->is_null())
        s.extras = *it;
    else s.extras.reset();
}

// ---- execution status -----------------------------------------------------

// The typed form of the legacy execution_status() record. exit_code engages
// only once the process has exited; the contract here omits it when empty
// (the legacy JSON wrote null — a presentation choice this contract drops).
struct ProcessExecution {
    ProcessState state = ProcessState::Unknown;
    std::optional<int> exit_code;
    std::uint64_t execution_milliseconds = 0;
};

inline void to_json(nlohmann::json& j, const ProcessExecution& e) {
    j = nlohmann::json{
        {"state", e.state},
        {"execution_milliseconds", e.execution_milliseconds},
    };
    if (e.exit_code) j["exit_code"] = *e.exit_code;
}

inline void from_json(const nlohmann::json& j, ProcessExecution& e) {
    if (auto it = j.find("state"); it != j.end()) it->get_to(e.state);
    if (auto it = j.find("execution_milliseconds"); it != j.end())
        it->get_to(e.execution_milliseconds);
    if (auto it = j.find("exit_code"); it != j.end() && !it->is_null())
        e.exit_code = it->get<int>();
    else e.exit_code.reset();
}

// ---- report ---------------------------------------------------------------

// One process as reported to a caller: identity (handle + label), how the
// execution went, and whichever output streams were captured. The stream
// fields carry a _text suffix because bare stdout/stderr collide with the
// <cstdio> macros of the same name; the wire keys keep the suffix for the
// usual keys-match-fields rule. Disengaged stream fields mean "not captured
// for this report" — a meta-only status listing, say — never "empty output".
struct ProcessReport {
    std::uint64_t id = 0;
    std::string description;
    ProcessExecution execution;
    std::optional<std::string> stdout_text;
    std::optional<std::string> stderr_text;
    std::optional<nlohmann::json> extras;
};

inline void to_json(nlohmann::json& j, const ProcessReport& r) {
    j = nlohmann::json{
        {"id", r.id},
        {"description", r.description},
        {"execution", r.execution},
    };
    if (r.stdout_text) j["stdout_text"] = *r.stdout_text;
    if (r.stderr_text) j["stderr_text"] = *r.stderr_text;
    if (r.extras) j["extras"] = *r.extras;
}

inline void from_json(const nlohmann::json& j, ProcessReport& r) {
    if (auto it = j.find("id"); it != j.end()) it->get_to(r.id);
    if (auto it = j.find("description"); it != j.end()) it->get_to(r.description);
    if (auto it = j.find("execution"); it != j.end()) it->get_to(r.execution);
    if (auto it = j.find("stdout_text"); it != j.end() && !it->is_null())
        r.stdout_text = it->get<std::string>();
    else r.stdout_text.reset();
    if (auto it = j.find("stderr_text"); it != j.end() && !it->is_null())
        r.stderr_text = it->get<std::string>();
    else r.stderr_text.reset();
    if (auto it = j.find("extras"); it != j.end() && !it->is_null())
        r.extras = *it;
    else r.extras.reset();
}

} // namespace process_io
