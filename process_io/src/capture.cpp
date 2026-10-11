#include "process_io/capture.hpp"

#include <algorithm>
#include <stdexcept>

namespace process_io {
namespace {

std::size_t index(Stream stream)
{
    switch (stream) {
        case Stream::Stdout: return 0;
        case Stream::Stderr: return 1;
        case Stream::Terminal: return 2;
    }
    throw std::invalid_argument("unknown process output stream");
}

} // namespace

CaptureBuffer::CaptureBuffer(OutputTopology topology, std::size_t max_output_bytes)
    : _limit(max_output_bytes)
{
    switch (topology) {
        case OutputTopology::Separate:
            _streams[0].emplace();
            _streams[1].emplace();
            break;
        case OutputTopology::Terminal:
            _streams[2].emplace();
            break;
        default:
            throw std::invalid_argument("unknown process output topology");
    }
}

CaptureBuffer::Record& CaptureBuffer::require(Stream stream)
{
    auto& record = _streams[index(stream)];
    if (!record) {
        throw std::invalid_argument("output stream does not exist in this environment");
    }
    return *record;
}

std::optional<StreamView> CaptureBuffer::view(Stream stream) const
{
    const auto& record = _streams[index(stream)];
    if (!record) {
        return std::nullopt;
    }
    return StreamView{record->bytes, record->truncated, record->end, record->error};
}

const std::string& CaptureBuffer::bytes(Stream stream) const
{
    const auto& record = _streams[index(stream)];
    if (!record) {
        throw std::invalid_argument("output stream does not exist in this environment");
    }
    return record->bytes;
}

void CaptureBuffer::append(Stream stream, std::string_view bytes)
{
    auto& record = require(stream);
    if (record.end != StreamEnd::Open) {
        throw std::logic_error("cannot append to an ended output stream");
    }
    const auto count = _limit == 0
        ? bytes.size()
        : std::min(bytes.size(), _limit - record.bytes.size());
    record.bytes.append(bytes.substr(0, count));
    record.truncated |= count != bytes.size();
}

void CaptureBuffer::finish(Stream stream, StreamEnd end, boost::system::error_code error)
{
    if (end == StreamEnd::Open) {
        throw std::invalid_argument("finishing capture requires an end reason");
    }
    auto& record = require(stream);
    if (record.end != StreamEnd::Open) {
        return;
    }
    record.end = end;
    record.error = error;
    record.truncated |= end == StreamEnd::Stopped || end == StreamEnd::Failed;
}

bool CaptureBuffer::complete() const noexcept
{
    return std::all_of(_streams.begin(), _streams.end(), [](const auto& record) {
        return !record || record->end != StreamEnd::Open;
    });
}

} // namespace process_io
