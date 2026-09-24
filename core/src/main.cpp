#include "core/application.hpp"
#include "core/protocol.hpp"
#include <csignal>
#include <iostream>

/** CLI owns the executor until controlled worker shutdown has joined all tasks. */
int main(int argc, char** argv) {
    try {
        std::string file = "config.yaml";
        std::string session;
        for (int i = 1; i < argc; ++i) {
            const std::string flag = argv[i];
            if (flag == "--help") {
                std::cout << "simplex_worker --config FILE --session ID\n";
                return 0;
            }
            if ((flag != "--config" && flag != "--session") || i + 1 == argc)
                throw std::invalid_argument("expected --config FILE or --session ID");
            (flag == "--config" ? file : session) = argv[++i];
        }
        core::validate_session_id(session);
        auto config = load::read_configuration(file);
        boost::asio::io_context context;
        core::Application app(context.get_executor(), std::move(config), session);
        boost::asio::signal_set signals(context, SIGINT, SIGTERM);
        signals.async_wait([&](boost::system::error_code error, int) {
            if (!error) app.stop();
        });
        std::exception_ptr failure;
        boost::asio::co_spawn(context, app.run(), [&](std::exception_ptr error) {
            failure = error;
            signals.cancel();
        });
        context.run();
        if (failure) std::rethrow_exception(failure);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Worker: " << error.what() << '\n';
        return 1;
    }
}
