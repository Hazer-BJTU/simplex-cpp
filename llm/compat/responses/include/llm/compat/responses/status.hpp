#pragma once

#include <iosfwd>

namespace llm::responses {

/**
 * Terminal state of one streamed response. Streaming is the pre-terminal
 * default; Completed..Errored are the wire's own terminal events; Aborted
 * is this layer's addition for a stream that ended WITHOUT a terminal
 * event (transport fault, external finish()/abort(), reader-side fault).
 */
enum class ResponseStatus {
    Streaming,
    Completed,
    Incomplete,
    Failed,
    Errored,
    Cancelled,
    Aborted,
};

std::ostream& operator<<(std::ostream& os, ResponseStatus status);

} // namespace llm::responses
