#include "subprocess/child.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <string_view>
#include <boost/process.hpp>
#include <boost/process/environment.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace subprocess {
namespace {

constexpr std::string_view env_key(std::string_view entry)
{
    return entry.substr(0, entry.find('='));
}

} // namespace

struct IoAttachment::Impl {
    boost::asio::writable_pipe& input;
    boost::asio::readable_pipe& output;
    boost::asio::readable_pipe& error;
};

IoAttachment::IoAttachment(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}
IoAttachment::~IoAttachment() = default;
IoAttachment::IoAttachment(IoAttachment&&) noexcept = default;
IoAttachment& IoAttachment::operator=(IoAttachment&&) noexcept = default;

IoAttachment IoAttachment::pipes(
    boost::asio::writable_pipe& input,
    boost::asio::readable_pipe& output,
    boost::asio::readable_pipe& error)
{
    return IoAttachment(std::make_unique<Impl>(input, output, error));
}

struct Child::Impl {
    std::unique_ptr<boost::process::process> process;
};

Child::Child(const process::LaunchSpec& spec,
             boost::asio::any_io_executor executor,
             const IoAttachment& attachment)
    : _impl(std::make_unique<Impl>())
{
    if (!attachment._impl) {
        throw ProcessException(ProcessException::Stage::Spawn,
            "launch attachment has been moved from", {}, spec.executable,
            spec.description);
    }
    // Assemble the child's environment BEFORE resolving anything: a
    // spec-supplied PATH must steer the lookup, and a malformed entry is a
    // launch failure the caller can act on, not a surprise inside the
    // child's environ.
    //
    // The assembly is a MERGE, never a concatenation. execve entries with
    // duplicated names are undefined (glibc's getenv returns the FIRST
    // match, so "inherit FOO=old, append FOO=new" would hand the child
    // "old"), and v2's process_environment passes entries through verbatim
    // without deduplicating — so uniqueness is this constructor's job: an
    // explicit entry replaces the inherited entry of the same key, a later
    // explicit entry replaces an earlier one.
    std::vector<boost::process::environment::key_value_pair> used_envs;
    if (spec.inherit_environment) {
        auto current_env = boost::process::environment::current();
        used_envs.assign(current_env.begin(), current_env.end());
    }
    if (spec.environment) {
        for (const auto& env_config: *spec.environment) {
            const auto eq = env_config.find('=');
            if (env_config.empty() || eq == std::string::npos || eq == 0 ||
                env_config.find('\0') != std::string::npos) {
                throw ProcessException(
                    ProcessException::Stage::Environment,
                    std::format(
                        "invalid environment entry, expected KEY=VALUE: \"{}\"",
                        env_config),
                    {},
                    spec.executable,
                    spec.description
                );
            }
            const auto key = env_key(env_config);
            std::erase_if(used_envs, [&key](const auto& kv) {
                const std::string_view existing{kv.data(), kv.size()};
                return env_key(existing) == key;
            });
            used_envs.emplace_back(env_config);
        }
    }

    // Executable resolution runs against the SAME environment the child
    // will get — a spec-supplied PATH is honored (including with
    // inherit_environment=false, where the spec's PATH is the only one).
    // When the assembled environment carries no PATH at all, resolution
    // falls back to the PARENT's PATH: a documented convenience so bare
    // names still resolve; the child's own environment is unaffected by
    // the fallback and really does ship without a PATH.
    const bool child_env_has_path = std::any_of(
        used_envs.begin(), used_envs.end(), [](const auto& kv) {
            return std::string_view{kv.data(), kv.size()}
                .starts_with("PATH=");
        });

    // The PATH half of the rule below, against whichever environment the
    // comment above selected.
    const auto search_path = [&](const std::filesystem::path& name) {
        const boost::process::v2::filesystem::path lookup(name.string());
        return child_env_has_path
            ? boost::process::environment::find_executable(lookup, used_envs)
            : boost::process::environment::find_executable(lookup);
    };

    // Paths identify exactly one program; only bare names use PATH. Anchor
    // relative paths (including PATH search results) to the host cwd before
    // the child changes working_directory, so launch cannot select a different
    // executable after confirmation.
    const std::filesystem::path requested{spec.executable};
    auto exec_path = requested.has_parent_path()
        ? boost::process::v2::filesystem::path(requested.string())
        : search_path(requested);
    std::error_code resolve_ec;
    if (!exec_path.empty()) {
        const auto absolute = std::filesystem::absolute(
            std::filesystem::path(exec_path.string()), resolve_ec);
        if (!resolve_ec) {
            exec_path = boost::process::v2::filesystem::path(absolute.string());
        }
    }
    if (exec_path.empty() || resolve_ec ||
        !std::filesystem::exists(std::filesystem::path(exec_path.string()),
                                 resolve_ec)) {
        throw ProcessException(
            ProcessException::Stage::ResolveExecutable,
            "executable not found or path could not be resolved",
            resolve_ec,
            spec.executable,
            spec.description
        );
    }

    // The start directory is checked HERE rather than left to the launch: v2
    // applies it as a chdir in the forked child, and a failure there arrives
    // as a bare ENOENT with nothing saying which path was meant — the launch
    // context this exception exists to carry. Checked with the non-throwing
    // overload: a filesystem error (an unreadable parent, a broken symlink)
    // is reported as the launch failure it is, not as a std::filesystem
    // exception escaping a constructor.
    if (spec.working_directory) {
        std::error_code dir_ec;
        const std::filesystem::path start_dir{*spec.working_directory};
        if (!std::filesystem::is_directory(start_dir, dir_ec)) {
            throw ProcessException(
                ProcessException::Stage::Spawn,
                std::format(
                    "working directory is not a directory: \"{}\"",
                    *spec.working_directory),
                {},
                spec.executable,
                spec.description
            );
        }
    }

    try {
        // The start directory rides as one more v2 initializer, which is why
        // the two spawn forms are spelled out instead of building a pack: the
        // process constructor takes its initializers as variadic arguments,
        // so an engaged optional cannot simply be "passed as nothing".
        if (spec.working_directory) {
            _impl->process = std::make_unique<boost::process::process>(
                executor,
                std::move(exec_path),
                spec.arguments,
                boost::process::process_stdio{
                    attachment._impl->input,
                    attachment._impl->output,
                    attachment._impl->error},
                boost::process::process_environment(used_envs),
                // From the string, not a path object: this Boost builds
                // Process against boost::filesystem, while the check above
                // (and the rest of the tree) speaks std::filesystem — the
                // two path types do not convert, and the initializer's own
                // constructor is happy to build its path from the string.
                boost::process::process_start_dir(*spec.working_directory)
            );
        } else {
            _impl->process = std::make_unique<boost::process::process>(
                executor,
                std::move(exec_path),
                spec.arguments,
                boost::process::process_stdio{
                    attachment._impl->input,
                    attachment._impl->output,
                    attachment._impl->error},
                boost::process::process_environment(used_envs)
            );
        }

    } catch(const std::exception& e) {
        throw ProcessException(
            ProcessException::Stage::Spawn,
            e.what(),
            {},
            spec.executable,
            spec.description
        );
    } catch(...) {
        throw ProcessException(
            ProcessException::Stage::Spawn,
            "unknown",
            {},
            spec.executable,
            spec.description
        );
    }
}

Child::~Child() = default;

process::ProcessId Child::id() const noexcept
{
    return static_cast<process::ProcessId>(_impl->process->id());
}

bool Child::running(boost::system::error_code& error)
{
    return _impl->process->running(error);
}

boost::asio::awaitable<int> Child::wait(boost::system::error_code& error)
{
    co_return co_await _impl->process->async_wait(
        boost::asio::redirect_error(boost::asio::use_awaitable, error));
}

void Child::terminate(boost::system::error_code& error)
{
    _impl->process->terminate(error);
}

void Child::request_exit(boost::system::error_code& error)
{
    _impl->process->request_exit(error);
}

int Child::exit_code() const
{
    return _impl->process->exit_code();
}

boost::system::error_code Child::emergency_cleanup() noexcept
{
    try {
        boost::system::error_code error;
        if (_impl->process->running(error)) {
            _impl->process->terminate(error);
        }
        return error;
    } catch (const boost::system::system_error& error) {
        return error.code();
    } catch (...) {
        return boost::system::errc::make_error_code(boost::system::errc::io_error);
    }
}

} // namespace subprocess
