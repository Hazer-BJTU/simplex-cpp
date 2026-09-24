#include "intercom/cancellable_exchange.hpp"
#include <boost/asio/experimental/channel.hpp>

namespace intercom {
namespace asio = boost::asio;
namespace {
struct Operation {
    explicit Operation(asio::any_io_executor executor)
        : timer(executor), timer_done(executor, 1) {}
    asio::steady_timer timer;
    asio::experimental::channel<void(boost::system::error_code, bool)> timer_done;
    asio::cancellation_signal cancellation;
    std::optional<websocket_stream> stream;
    bool active = true;
    bool expired = false;

    void abort() {
        if (!active) return;
        cancellation.emit(asio::cancellation_type::terminal);
        if (stream) stream->abort();
    }
};
}

asio::awaitable<std::string> cancellable_exchange(
    asio::any_io_executor executor, endpoint::ResolvedEndpoint endpoint,
    std::string request, std::chrono::milliseconds timeout,
    std::stop_token stop, endpoint::ssl_context& context) {
    co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
    if (timeout.count() <= 0) throw std::invalid_argument("exchange timeout must be positive");
    auto strand = asio::make_strand(executor);
    auto operation = std::make_shared<Operation>(strand);
    auto supervise = [operation, strand, endpoint, request = std::move(request),
                      timeout, stop, &context]() mutable -> asio::awaitable<std::string> {
        std::stop_callback on_stop(stop, [operation, strand] {
            asio::post(strand, [operation] { operation->abort(); });
        });
        operation->timer.expires_after(timeout);
        operation->timer.async_wait([operation](boost::system::error_code error) {
            if (!error && operation->active) {
                operation->expired = true;
                operation->abort();
            }
            operation->timer_done.try_send(boost::system::error_code{}, true);
        });
        auto exchange = [operation, strand, endpoint, request = std::move(request),
                         stop, &context]() mutable -> asio::awaitable<std::string> {
            if (stop.stop_requested() || operation->expired)
                throw boost::system::system_error(asio::error::operation_aborted);
            operation->stream.emplace(co_await connect_websocket(strand, endpoint, context));
            if (stop.stop_requested() || operation->expired)
                throw boost::system::system_error(asio::error::operation_aborted);
            co_await operation->stream->write(std::move(request));
            boost::beast::flat_buffer buffer;
            co_await operation->stream->read(buffer);
            if (!operation->stream->got_text())
                throw WsProtocolException("binary confirmation reply", endpoint.host, endpoint.target);
            auto reply = boost::beast::buffers_to_string(buffer.data());
            co_await operation->stream->close();
            co_return reply;
        };
        std::exception_ptr failure;
        std::string reply;
        try {
            reply = co_await asio::co_spawn(strand, std::move(exchange),
                asio::bind_cancellation_slot(operation->cancellation.slot(), asio::use_awaitable));
        } catch (...) {
            failure = std::current_exception();
        }
        operation->active = false;
        if (operation->stream) operation->stream->abort();
        operation->timer.cancel();
        co_await operation->timer_done.async_receive(asio::use_awaitable);
        if (operation->expired) throw boost::system::system_error(asio::error::timed_out);
        if (stop.stop_requested()) throw boost::system::system_error(asio::error::operation_aborted);
        if (failure) std::rethrow_exception(failure);
        co_return reply;
    };
    co_return co_await asio::co_spawn(strand, std::move(supervise), asio::use_awaitable);
}
} // namespace intercom
