// Shared test support for the socket-level intercom tests: loopback WebSocket
// servers. Deterministic and offline — everything runs against 127.0.0.1 over
// plain ws:// (no TLS) with fixed exchanges, no timing dependence except where
// a test deliberately drives the read-deadline case. The server publishes its
// port as soon as it is listening and rethrows any server-side failure on
// join(), so a broken test fails loudly instead of hanging the client.
#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

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
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

namespace loopback_ws {

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
// connection in order — for retry-engine tests whose client makes a known
// number of connections in a known order. The sequence length IS the expected
// connection count; a shortfall must not wedge the suite, so a grace deadline
// bounds the wait for the next expected connection.
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

// --- responder flavours -------------------------------------------------------
//
// Each accepts the WebSocket upgrade, reads the client's one message, and then
// answers (or not) per the flavour. The websocket::stream<tcp::socket> holds
// the socket for the callback's duration and closes it on destruction.

// Echo the client's message back as a single text message. When got_text_out
// is non-null it records whether the CLIENT's message was a text frame — the
// assertion point for the module's "text by default" contract.
inline void serve_echo(tcp::socket& socket, bool* got_text_out = nullptr) {
    websocket::stream<tcp::socket> ws{std::move(socket)};
    ws.accept();
    beast::flat_buffer buffer;
    ws.read(buffer);
    if (got_text_out) *got_text_out = ws.got_text();
    ws.text(true);
    const std::string reply = beast::buffers_to_string(buffer.data());
    ws.write(asio::buffer(reply));
}

// Read the client's message, then reply with a fixed string.
inline void serve_reply(tcp::socket& socket, const std::string& reply) {
    websocket::stream<tcp::socket> ws{std::move(socket)};
    ws.accept();
    beast::flat_buffer buffer;
    ws.read(buffer);
    ws.text(true);
    ws.write(asio::buffer(reply));
}

// Read the client's message, then reply with TWO fragments of one message —
// the reassembly case (the client's read must concatenate them).
inline void serve_fragmented(
    tcp::socket& socket, const std::string& part1, const std::string& part2) {
    websocket::stream<tcp::socket> ws{std::move(socket)};
    ws.accept();
    beast::flat_buffer buffer;
    ws.read(buffer);
    ws.text(true);
    ws.write_some(false, asio::buffer(part1));   // first fragment, not final
    ws.write_some(true, asio::buffer(part2));    // final fragment
}

// Reject the upgrade with an HTTP status instead of switching protocols — the
// handshake-rejection case (the client's connect_websocket folds this into
// WsException{Connect} with error::upgrade_declined).
inline void serve_reject(
    tcp::socket& socket, http::status status, const std::string& body) {
    beast::flat_buffer buffer;
    http::request<http::string_body> request;
    http::read(socket, buffer, request);

    http::response<http::string_body> response(status, 11);
    response.body() = body;
    response.prepare_payload();
    http::write(socket, response);
}

// Accept and read the client's message, then drop the session without
// replying — the transient mid-exchange failure a retry layer should recover
// from (the client's read ends in a transport error, stage Read).
inline void serve_drop_after_read(tcp::socket& socket) {
    websocket::stream<tcp::socket> ws{std::move(socket)};
    ws.accept();
    beast::flat_buffer buffer;
    ws.read(buffer);
    // Return without replying; the ws destructor closes the socket.
}

// Accept and read the client's message, then hold the session open without
// replying for the given delay — the slow-backend case read deadlines exist
// for. Callers pick a delay longer than the client's read timeout so the
// timeout, not the close, is what the client observes.
inline void serve_hold_after_read(
    tcp::socket& socket, std::chrono::seconds delay) {
    websocket::stream<tcp::socket> ws{std::move(socket)};
    ws.accept();
    beast::flat_buffer buffer;
    ws.read(buffer);
    std::this_thread::sleep_for(delay);
}

} // namespace loopback_ws
