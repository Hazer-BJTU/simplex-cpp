#pragma once
#include <boost/process.hpp>
#include <boost/process/environment.hpp>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace subprocess {
/// Private adapter: adopt a successfully exec'd PTY child into Boost.Process.
/// All arguments, environment entries and the slave remain valid for this
/// synchronous call. The parent keeps slave ownership; a launch/adoption error
/// kills and reaps the forked child before propagating the original failure.
/// No C++ allocation or executor work is performed in the child after fork.
std::unique_ptr<boost::process::process> launch_terminal(
    boost::asio::any_io_executor executor,
    const boost::process::v2::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    const std::vector<boost::process::environment::key_value_pair>& environment,
    const std::optional<std::string>& directory,
    int slave);
}
