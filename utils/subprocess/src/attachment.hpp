#pragma once
#include "subprocess/child.hpp"
#include "subprocess/terminal.hpp"

namespace subprocess {
/// Private borrowed launch representation. Pipe launches use the three Asio
/// objects; terminal launches use Terminal exclusively. None is owned here.
struct IoAttachment::Impl {
    boost::asio::writable_pipe* input = nullptr;
    boost::asio::readable_pipe* output = nullptr;
    boost::asio::readable_pipe* error = nullptr;
    Terminal* terminal = nullptr;
};
}
