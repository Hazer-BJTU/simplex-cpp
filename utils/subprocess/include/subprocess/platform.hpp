#pragma once

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace subprocess {

/// A shell executable and the arguments preceding a command string. Keeping
/// the prefix as a vector avoids assuming a universal single command flag.
struct ShellInvocation {
    std::string executable;
    std::vector<std::string> argument_prefix;
};

/// Preserve the Linux policy: conventional bash locations, then /bin/sh.
/// Never resolve the interpreter through a caller-supplied child PATH.
[[nodiscard]] ShellInvocation platform_shell();

/// Absolute path of the running executable, not the library containing this
/// helper. Returns an empty path on lookup failure and supplies the error.
[[nodiscard]] std::filesystem::path executable_location(std::error_code& error);

/// Throwing lookup for callers whose installed assets are mandatory.
[[nodiscard]] std::filesystem::path executable_location();

} // namespace subprocess
