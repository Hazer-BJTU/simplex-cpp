#pragma once

#include <boost/system/error_code.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace process {

/**
 * Describes a failure in one stage of managing a child process and retains
 * enough launch context for callers to log or classify the failure.
 *
 * The design follows endpoint::HttpRequestException (the most mature
 * exception in this tree): what() already IS the full one-line rendering
 * (stage, message, error code, executable, description) that to_string()
 * documents — not just the bare message. That equivalence is deliberate: a
 * host that catches this type only as std::exception — the one dependable
 * catch across a dlopen boundary, where each module carries its own
 * typeinfo copy — still sees the whole context in what().
 *
 * One intended construction site: the spawn path. Boost.Process v2 reports a
 * failed launch (executable not found, fork/exec failure) by throwing
 * boost::system::system_error; the process manager translates that into this
 * type at the boundary so the rest of the tree never catches boost types.
 * Runtime pipe errors are boolean/channel signals by design and do not route
 * through exceptions.
 */
class ProcessException : public std::runtime_error {
public:
    enum class Stage {
        ResolveExecutable, // locating the executable (PATH lookup)
        Environment,       // assembling the child's environment (merge/validation)
        Spawn,             // launching the child process
        Terminate,         // terminating / reaping a running process
        Write,             // feeding the child's stdin
        Read,              // collecting the child's stdout/stderr
        Unknown
    };

    ProcessException(
        Stage stage,
        std::string message,
        boost::system::error_code ec = {},
        std::string executable = {},
        std::string description = {})
        : std::runtime_error(render(stage, message, ec, executable, description)),
          stage_(stage),
          ec_(ec),
          executable_(std::move(executable)),
          description_(std::move(description))
    {}

    [[nodiscard]] Stage stage() const noexcept { return stage_; }
    [[nodiscard]] const boost::system::error_code& error_code() const noexcept
    {
        return ec_;
    }
    [[nodiscard]] const std::string& executable() const noexcept
    {
        return executable_;
    }
    [[nodiscard]] const std::string& description() const noexcept
    {
        return description_;
    }

    /** The stage's phrase as rendered by to_string(): "while spawning the
     *  process", … */
    [[nodiscard]] static constexpr std::string_view stage_phrase(Stage stage) noexcept
    {
        switch (stage) {
            case Stage::ResolveExecutable: return "while resolving the executable";
            case Stage::Environment:       return "while assembling the child environment";
            case Stage::Spawn:             return "while spawning the process";
            case Stage::Terminate:         return "while terminating the process";
            case Stage::Write:             return "while writing to the process";
            case Stage::Read:              return "while reading from the process";
            case Stage::Unknown:           return "at an unknown stage";
        }
        return "at an unknown stage"; // unreachable; keeps -Wreturn-type quiet
    }

    /**
     * One-line, log-friendly rendering of the failure in prose rather than
     * key=value tags: the stage phrase and the message lead, and whichever
     * context fields are set gather into one parenthetical, e.g.
     *   Failed while resolving the executable: no such file \
     * (No such file or directory; executable wc; count lines)
     * Absent fields (no error code, no launch context) are omitted.
     *
     * Identical to what(): the rendering is baked in at construction (see
     * the class doc for why what() must carry it).
     */
    [[nodiscard]] std::string to_string() const
    {
        return what();
    }

private:
    /// The one-line rendering baked into the runtime_error base at
    /// construction, so what() carries the full context everywhere (the
    /// format is the one to_string() documents).
    static std::string render(
        Stage stage,
        const std::string& message,
        const boost::system::error_code& ec,
        const std::string& executable,
        const std::string& description)
    {
        std::string rendered = "Failed ";
        rendered += stage_phrase(stage);
        rendered += ": ";
        rendered += message;

        // Context pieces join into a single parenthetical, "; "-separated.
        bool opened = false;
        auto append = [&](std::string_view piece) {
            rendered += opened ? "; " : " (";
            opened = true;
            rendered += piece;
        };
        if (ec) append(ec.message());
        if (!executable.empty()) append("executable " + executable);
        if (!description.empty()) append(description);
        if (opened) rendered += ')';
        return rendered;
    }

    Stage stage_;
    boost::system::error_code ec_;
    std::string executable_;
    std::string description_;
};

} // namespace process
