#include "subprocess/platform.hpp"

#include <boost/dll/runtime_symbol_info.hpp>

namespace subprocess {

ShellInvocation platform_shell()
{
    for (const auto* candidate : {"/bin/bash", "/usr/bin/bash"}) {
        std::error_code ignored;
        if (std::filesystem::is_regular_file(candidate, ignored)) {
            return {candidate, {"-c"}};
        }
    }
    return {"/bin/sh", {"-c"}};
}

std::filesystem::path executable_location(std::error_code& error)
{
    return boost::dll::program_location(error);
}

std::filesystem::path executable_location()
{
    std::error_code error;
    auto result = executable_location(error);
    if (error) {
        throw std::filesystem::filesystem_error("locating running executable", error);
    }
    return result;
}

} // namespace subprocess
