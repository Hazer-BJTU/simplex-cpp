#pragma once

#include "subprocess/exception.hpp"

namespace process {

// Preserve the process API while low-level launch helpers own the error type.
using ProcessException = subprocess::ProcessException;

} // namespace process
