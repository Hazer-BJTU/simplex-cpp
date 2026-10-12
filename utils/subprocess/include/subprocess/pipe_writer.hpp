#pragma once

#include <memory>
#include <string_view>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/system/error_code.hpp>

namespace subprocess {

/** Owned pipe input transport. POSIX writes suppress only the calling thread's
 * newly generated SIGPIPE during each synchronous, nonblocking write attempt;
 * no process-wide signal disposition changes, and no mask crosses co_await.
 * All operations require the owner's serialization discipline. The caller must
 * join pending writes before destruction and keep the executor context alive.
 */
class PipeWriter {
public:
    explicit PipeWriter(boost::asio::any_io_executor executor);
    ~PipeWriter();
    PipeWriter(const PipeWriter&) = delete;
    PipeWriter& operator=(const PipeWriter&) = delete;

    /// Borrow solely to build an IoAttachment before prepare(). Child launch
    /// populates this pipe; it is transferred to the private writer on prepare.
    boost::asio::writable_pipe& launch_pipe();
    /// After launch, once only. Configure the private nonblocking transport.
    void prepare();
    [[nodiscard]] bool is_open() const noexcept;
    /// Close whichever transport owns the pipe, preserving the first close error.
    /// Outstanding readiness waits still need to finish before destruction.
    void close(boost::system::error_code& error) noexcept;
    /// Write all bytes or return a partial count with error. The string storage
    /// must outlive the operation. Closure cancels readiness waits and writes.
    boost::asio::awaitable<std::size_t>
    write(std::string_view bytes, boost::system::error_code& error);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace subprocess
