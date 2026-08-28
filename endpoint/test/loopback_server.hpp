// Shared test support for the socket-level endpoint tests: a one-shot loopback
// HTTP server. Deterministic and offline — everything runs against 127.0.0.1
// with fixed responses, no TLS, no timing dependence. The server publishes its
// port as soon as it is listening and rethrows any server-side failure on
// join(), so a broken test fails loudly instead of hanging the client.
#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

namespace loopback {

// Serve exactly one accepted client, synchronously, on the server thread. The
// socket is closed by the server after the callback returns.
using Serve = std::function<void(tcp::socket&)>;

class OneShotServer {
public:
    OneShotServer(Serve serve)
        : _port_promise(std::make_shared<std::promise<unsigned short>>())
        , _done_promise(std::make_shared<std::promise<void>>())
        , _port(_port_promise->get_future())
        , _done(_done_promise->get_future())
        , _thread([this, serve = std::move(serve)] {
              try {
                  asio::io_context io;
                  tcp::acceptor acceptor(
                      io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
                  _port_promise->set_value(acceptor.local_endpoint().port());

                  tcp::socket socket(io);
                  acceptor.accept(socket);
                  serve(socket);

                  boost::system::error_code ignored;
                  socket.shutdown(tcp::socket::shutdown_both, ignored);
                  socket.close(ignored);
                  _done_promise->set_value();
              } catch (...) {
                  try {
                      _done_promise->set_exception(std::current_exception());
                  } catch (...) {}
              }
          })
    {}

    OneShotServer(const OneShotServer&) = delete;
    OneShotServer& operator=(const OneShotServer&) = delete;

    ~OneShotServer() {
        if (_thread.joinable()) _thread.join();
    }

    unsigned short wait_listening() { return _port.get(); }
    void join() {
        _thread.join();
        _done.get();
    }

private:
    std::shared_ptr<std::promise<unsigned short>> _port_promise;
    std::shared_ptr<std::promise<void>> _done_promise;
    std::future<unsigned short> _port;
    std::future<void> _done;
    std::thread _thread;
};

// Serve a fixed SEQUENCE of accepted clients, synchronously, one Serve per
// connection in order — for tests whose client makes a known number of
// connections in a known order (provider_info's catalogue-then-balance
// pair). The sequence length IS the expected connection count: a shortfall
// must not wedge the suite, so the deadline bounds the wait for the next
// expected connection (the test's own assertions report what went missing);
// connections beyond the sequence are not accepted. Same port/done contract
// as OneShotServer.
class SequenceServer {
public:
    explicit SequenceServer(
        std::vector<Serve> sequence,
        std::chrono::seconds grace = std::chrono::seconds(5))
        : _port_promise(std::make_shared<std::promise<unsigned short>>())
        , _done_promise(std::make_shared<std::promise<void>>())
        , _port(_port_promise->get_future())
        , _done(_done_promise->get_future())
        , _thread([this, sequence = std::move(sequence), grace] {
              try {
                  asio::io_context io;
                  tcp::acceptor acceptor(
                      io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
                  _port_promise->set_value(acceptor.local_endpoint().port());

                  asio::steady_timer deadline(io, grace);
                  deadline.async_wait(
                      [&acceptor](const boost::system::error_code&) {
                          acceptor.cancel();
                      });

                  auto next = sequence.begin();
                  std::function<void()> accept_next = [&] {
                      if (next == sequence.end()) return;
                      auto socket = std::make_shared<tcp::socket>(io);
                      acceptor.async_accept(
                          *socket, [&, socket](
                                       const boost::system::error_code& ec) {
                              if (ec) return;   // cancelled: time is up
                              (*next)(*socket);
                              ++next;
                              boost::system::error_code ignored;
                              socket->shutdown(tcp::socket::shutdown_both,
                                               ignored);
                              socket->close(ignored);
                              // Sequence complete: stop the grace deadline so
                              // io.run() (and join()) return promptly.
                              if (next == sequence.end()) deadline.cancel();
                              accept_next();
                          });
                  };
                  accept_next();
                  io.run();
                  _done_promise->set_value();
              } catch (...) {
                  try {
                      _done_promise->set_exception(std::current_exception());
                  } catch (...) {}
              }
          })
    {}

    SequenceServer(const SequenceServer&) = delete;
    SequenceServer& operator=(const SequenceServer&) = delete;

    ~SequenceServer() {
        if (_thread.joinable()) _thread.join();
    }

    unsigned short wait_listening() { return _port.get(); }
    void join() {
        _thread.join();
        _done.get();
    }

private:
    std::shared_ptr<std::promise<unsigned short>> _port_promise;
    std::shared_ptr<std::promise<void>> _done_promise;
    std::future<unsigned short> _port;
    std::future<void> _done;
    std::thread _thread;
};

// Responder flavour: read the client's request, answer once with the given
// status, headers, and body, then let the server close the connection.
inline void serve_fixed_response(
    tcp::socket& socket,
    http::status status,
    const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& headers = {})
{
    beast::flat_buffer buffer;
    http::request<http::string_body> request;
    http::read(socket, buffer, request);

    http::response<http::string_body> response(status, 11);
    response.set(http::field::content_type, "text/event-stream");
    response.set(http::field::connection, "close");
    for (const auto& [name, value] : headers) {
        response.set(name, value);
    }
    response.body() = body;
    response.prepare_payload();
    http::write(socket, response);
}

// Responder flavour: read the client's request, then close without replying —
// the transport-failure case (peer drops the connection mid-exchange).
inline void serve_close_without_response(tcp::socket& socket)
{
    beast::flat_buffer buffer;
    http::request<http::string_body> request;
    http::read(socket, buffer, request);
    // Just return; the server closes the socket right after.
}

// Responder flavour: read the client's request, then hold the connection open
// without responding for the given delay — the slow-backend case read
// deadlines exist for. Callers pick a delay longer than the client's read
// timeout so the timeout, not the close, is what the client observes.
inline void serve_silent_delay(tcp::socket& socket, std::chrono::seconds delay)
{
    beast::flat_buffer buffer;
    http::request<http::string_body> request;
    http::read(socket, buffer, request);
    std::this_thread::sleep_for(delay);
}

} // namespace loopback
