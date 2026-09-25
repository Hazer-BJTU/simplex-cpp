#include "terminal.hpp"
#include "core/protocol.hpp"
#include <boost/asio.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/beast.hpp>
#include <csignal>
#include <map>
#include <memory>
#include <sstream>
#include <set>
#include <unistd.h>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ws = beast::websocket;
using Json = nlohmann::json;
using Socket = ws::stream<beast::tcp_stream>;
using Channel = asio::experimental::channel<void(boost::system::error_code, Json)>;

namespace {
/** All shell state lives on one io_context thread, including console output. */
struct Server {
    asio::io_context& io;
    asio::ip::tcp::acceptor acceptor;
    asio::posix::stream_descriptor input;
    asio::signal_set signals;
    asio::experimental::channel<void(boost::system::error_code, bool)> done;
    std::string events_path;
    std::string confirmation_path;
    std::string session_id;
    std::string run_id;
    std::string worker_id;
    std::shared_ptr<Socket> worker;
    std::shared_ptr<Channel> outgoing;
    std::map<std::string, std::shared_ptr<Channel>> prompts;
    std::set<std::string> answered;
    std::map<std::string, std::string> prompt_runs;
    std::map<std::size_t, std::shared_ptr<Socket>> sockets;
    std::size_t socket_sequence = 0;
    std::size_t tasks = 0;
    bool stopping = false;

    Server(asio::io_context& context, asio::ip::tcp::endpoint endpoint,
           std::string events, std::string confirmation)
        : io(context), acceptor(io, endpoint), input(io, duplicate_input()),
          signals(io, SIGINT, SIGTERM), done(io, 128),
          events_path(std::move(events)), confirmation_path(std::move(confirmation)) {
        if (events_path.empty() || confirmation_path.empty()
            || events_path.front() != '/' || confirmation_path.front() != '/'
            || events_path == confirmation_path)
            throw std::invalid_argument("routes must be distinct absolute paths");
    }

    static int duplicate_input() {
        int fd = ::dup(STDIN_FILENO);
        if (fd < 0) throw std::runtime_error("cannot duplicate stdin");
        return fd;
    }
    static void abort(Socket& socket) {
        boost::system::error_code ignored;
        beast::get_lowest_layer(socket).socket().close(ignored);
    }

    void stop() {
        if (stopping) return;
        stopping = true;
        boost::system::error_code ignored;
        acceptor.close(ignored);
        input.cancel(ignored);
        signals.cancel();
        for (auto& [id, socket] : sockets) abort(*socket);
        if (outgoing) outgoing->close();
        for (auto& [id, channel] : prompts) channel->close();
    }

    /** Task completion count is the fence before Server can be destroyed. */
    void spawn(asio::awaitable<void> task, bool critical = false) {
        ++tasks;
        asio::co_spawn(io, std::move(task), [this, critical](std::exception_ptr error) {
            if (error && !stopping) {
                try { std::rethrow_exception(error); }
                catch (const std::exception& e) { core_example::block("Connection", e.what()); }
            }
            if (error && critical) stop();
            --tasks;
            done.try_send(boost::system::error_code{}, true);
        });
    }

    asio::awaitable<void> write_events(std::shared_ptr<Socket> socket, std::shared_ptr<Channel> queue) {
        try {
            for (;;) {
                const auto value = co_await queue->async_receive(asio::use_awaitable);
                const auto wire = value.dump();
                socket->text(true);
                co_await socket->async_write(asio::buffer(wire), asio::use_awaitable);
            }
        } catch (...) {
            abort(*socket);
        }
    }

    asio::awaitable<void> events(std::shared_ptr<Socket> socket) {
        worker = socket;
        auto queue = std::make_shared<Channel>(io, 64);
        outgoing = queue;
        spawn(write_events(socket, queue));
        try {
            for (;;) {
                beast::flat_buffer buffer;
                co_await socket->async_read(buffer, asio::use_awaitable);
                if (!socket->got_text()) throw std::invalid_argument("binary worker message");
                const auto message = Json::parse(beast::buffers_to_string(buffer.data()));
                if (message.at("type") != "event") throw std::invalid_argument("expected worker event");
                const auto session = message.at("session_id").get<std::string>();
                const auto identity = message.at("worker_id").get<std::string>();
                core::validate_session_id(session);
                if (identity.empty()) throw std::invalid_argument("missing worker identity");
                if (!session_id.empty() && session_id != session)
                    throw std::invalid_argument("shell is bound to another session");
                if (!worker_id.empty() && worker_id != identity && !prompts.empty())
                    throw std::invalid_argument("previous worker still has confirmation prompts");
                session_id = session;
                worker_id = identity;
                run_id = message.at("run_id").get<std::string>();
                core_example::display(message);
            }
        } catch (const std::exception& error) {
            core_example::block("Worker disconnected", error.what());
        }
        queue->close();
        abort(*socket);
        if (worker == socket) {
            worker.reset();
            outgoing.reset();
        }
    }

    /** A dedicated reader notices disconnect/close even while an operator waits. */
    asio::awaitable<void> watch_confirmation(std::shared_ptr<Socket> socket,
                                            std::shared_ptr<Channel> answer,
                                            std::shared_ptr<Channel> finished) {
        try {
            beast::flat_buffer buffer;
            co_await socket->async_read(buffer, asio::use_awaitable);
            // A second application frame violates the single-request contract.
            abort(*socket);
        } catch (...) {}
        answer->close();
        finished->try_send(boost::system::error_code{}, Json());
    }

    asio::awaitable<void> confirmation(std::shared_ptr<Socket> socket) {
        auto deadline = std::make_shared<asio::steady_timer>(io, std::chrono::seconds(120));
        deadline->async_wait([socket, deadline](boost::system::error_code error) {
            if (!error) abort(*socket);
        });
        std::string id;
        auto answer = std::make_shared<Channel>(io, 1);
        auto finished = std::make_shared<Channel>(io, 1);
        bool watching = false;
        try {
            beast::flat_buffer buffer;
            co_await socket->async_read(buffer, asio::use_awaitable);
            if (!socket->got_text()) throw std::invalid_argument("binary confirmation");
            auto request = Json::parse(beast::buffers_to_string(buffer.data()));
            auto data = request.at("data");
            if (request.at("type") != "confirmation_request"
                || session_id.empty() || data.at("session_id") != session_id
                || data.at("worker_id").get<std::string>().empty()
                || (!worker_id.empty() && data.at("worker_id") != worker_id)
                || data.at("run_id").get<std::string>().empty())
                throw std::invalid_argument("confirmation is not from the active run");
            // Independent sockets need not preserve event-channel ordering.
            // A confirmation may arrive before run_started reaches this peer.
            run_id = data.at("run_id").get<std::string>();
            id = data.at("confirmation_id").get<std::string>();
            if (id.empty() || prompts.contains(id)) throw std::invalid_argument("duplicate confirmation ID");
            prompts.emplace(id, answer);
            prompt_runs.emplace(id, run_id);
            core_example::block("Confirm " + id, data.at("call").dump(2)
                + "\n/approve " + id + "   or   /deny " + id);
            spawn(watch_confirmation(socket, answer, finished));
            watching = true;
            const auto decision = co_await answer->async_receive(asio::use_awaitable);
            data.erase("call");
            data["decision"] = decision;
            data["reason"] = "terminal operator decision";
            const auto wire = Json({{"type", "confirmation_response"}, {"data", data}}).dump();
            socket->text(true);
            co_await socket->async_write(asio::buffer(wire), asio::use_awaitable);
            // The reader completes the worker's close handshake; deadline stays armed.
        } catch (const std::exception& error) {
            core_example::block("Confirmation ended", error.what());
            abort(*socket);
        }
        if (watching) co_await finished->async_receive(asio::use_awaitable);
        if (!id.empty()) {
            auto found = prompts.find(id);
            if (found != prompts.end() && found->second == answer) {
                prompts.erase(found);
                answered.erase(id);
                prompt_runs.erase(id);
            }
        }
        deadline->cancel();
        abort(*socket);
    }

    asio::awaitable<void> route(std::shared_ptr<Socket> socket, std::size_t id) {
        try {
            beast::get_lowest_layer(*socket).expires_after(std::chrono::seconds(10));
            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            co_await http::async_read(socket->next_layer(), buffer, request, asio::use_awaitable);
            const bool event_route = request.target() == events_path;
            const bool confirm_route = request.target() == confirmation_path;
            if (!ws::is_upgrade(request) || (!event_route && !confirm_route) || (event_route && worker)) {
                http::response<http::string_body> response{
                    event_route && worker ? http::status::conflict : http::status::not_found, request.version()};
                response.body() = "Unknown route or worker already connected";
                response.prepare_payload();
                co_await http::async_write(socket->next_layer(), response, asio::use_awaitable);
            } else {
                // Reserve before the handshake suspends, closing double-admission races.
                if (event_route) worker = socket;
                beast::get_lowest_layer(*socket).expires_never();
                auto timeout = ws::stream_base::timeout::suggested(beast::role_type::server);
                timeout.idle_timeout = ws::stream_base::none();
                socket->set_option(timeout);
                socket->read_message_max(16 * 1024 * 1024);
                co_await socket->async_accept(request, asio::use_awaitable);
                if (event_route) co_await events(socket);
                else co_await confirmation(socket);
            }
        } catch (const std::exception& error) {
            if (!stopping) core_example::block("Route error", error.what());
        }
        if (worker == socket) worker.reset();
        abort(*socket);
        sockets.erase(id);
    }

    asio::awaitable<void> accept() {
        while (!stopping) {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            if (sockets.size() >= 48) {
                socket.close();
                continue;
            }
            auto stream = std::make_shared<Socket>(std::move(socket));
            auto id = ++socket_sequence;
            sockets.emplace(id, stream);
            spawn(route(stream, id));
        }
    }

    asio::awaitable<void> terminal() {
        std::string buffer;
        try {
            while (!stopping) {
                const auto size = co_await asio::async_read_until(
                    input, asio::dynamic_buffer(buffer, 1024 * 1024), '\n', asio::use_awaitable);
                auto line = buffer.substr(0, size - 1);
                buffer.erase(0, size);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line == "/quit") { stop(); break; }
                if (line.starts_with("/approve ") || line.starts_with("/deny ")) {
                    const bool approve = line.starts_with("/approve ");
                    const auto id = line.substr(approve ? 9 : 6);
                    auto found = prompts.find(id);
                    if (found == prompts.end() || answered.contains(id) || !found->second->try_send(
                        boost::system::error_code{}, Json(approve ? "approved" : "denied")))
                        core_example::block("Expired prompt", id);
                    // Remove admission to a second answer immediately.
                    if (found != prompts.end()) answered.insert(id);
                    continue;
                }
                Json message;
                if (line == "/cancel" || line == "/status" ||
                    line == "/options" || line == "/shutdown") {
                    message = {{"type", "signal"}, {"data", {
                        {"operation", line.substr(1)},
                        // Independent approval/event sockets can reorder observations.
                        // A visible prompt provides the exact run being cancelled.
                        {"run_id", line == "/cancel" && !prompt_runs.empty()
                            ? prompt_runs.begin()->second : run_id}}}};
                } else if (!line.empty()) {
                    message = {{"type", "payload"}, {"data", {
                        {"operation", line == "/continue" ? "continue" : "message"},
                        {"request_id", core::new_identity()}}}};
                    if (line != "/continue") {
                        message["data"]["content"] = Json::array({{
                            {"type", "text"}, {"raw", line}
                        }});
                    }
                } else continue;
                if (!outgoing || !outgoing->try_send(boost::system::error_code{}, std::move(message)))
                    core_example::block("Not sent", "Worker offline or outgoing queue full; input was not queued.");
            }
        } catch (...) { stop(); }
    }

    asio::awaitable<void> run() {
        signals.async_wait([this](boost::system::error_code error, int) { if (!error) stop(); });
        spawn(accept(), true);
        spawn(terminal());
        while (tasks) co_await done.async_receive(asio::use_awaitable);
    }
};
}

int main(int argc, char** argv) {
    try {
        std::string listen = "127.0.0.1:8765";
        std::string events = "/agent/events";
        std::string confirmation = "/agent/confirm";
        for (int i = 1; i < argc; ++i) {
            const std::string flag = argv[i];
            if (flag == "--help") {
                std::cout << "simplex_shell --listen IP:PORT --events-path /agent/events "
                             "--confirmation-path /agent/confirm\n"
                             "/continue /cancel /status /options /shutdown /quit /approve ID /deny ID\n";
                return 0;
            }
            if (i + 1 == argc) throw std::invalid_argument("missing option value");
            if (flag == "--listen") listen = argv[++i];
            else if (flag == "--events-path") events = argv[++i];
            else if (flag == "--confirmation-path") confirmation = argv[++i];
            else throw std::invalid_argument("unknown shell option");
        }
        const auto colon = listen.rfind(':');
        if (colon == std::string::npos) throw std::invalid_argument("listen requires IP:PORT");
        const auto port_text = listen.substr(colon + 1);
        std::size_t consumed = 0;
        const auto port = std::stoul(port_text, &consumed);
        if (consumed != port_text.size() || port == 0 || port > 65535)
            throw std::invalid_argument("invalid listen port");
        asio::io_context io;
        Server server(io, {asio::ip::make_address(listen.substr(0, colon)),
            static_cast<unsigned short>(port)}, events, confirmation);
        std::exception_ptr failure;
        asio::co_spawn(io, server.run(), [&](std::exception_ptr error) { failure = error; server.stop(); });
        core_example::block("Shell", "Waiting for one worker. /quit exits; /shutdown stops the worker.");
        io.run();
        if (failure) std::rethrow_exception(failure);
    } catch (const std::exception& error) {
        std::cerr << "Shell: " << error.what() << '\n';
        return 1;
    }
}
