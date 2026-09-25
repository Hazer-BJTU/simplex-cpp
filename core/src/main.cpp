#include "core/application.hpp"
#include "core/protocol.hpp"

#include <boost/program_options.hpp>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

/** Parse startup options and own all executor threads through worker shutdown. */
int main(int argc, char** argv) {
    namespace asio = boost::asio;
    namespace options = boost::program_options;

    try {
        std::string file;
        std::string session;
        int thread_count = 1;
        options::options_description description("Worker options");
        description.add_options()
            ("help,h", "Show this help message")
            ("config,c", options::value<std::string>(&file)->default_value("config.yaml"),
                "Startup YAML configuration file")
            ("session,s", options::value<std::string>(&session)->required(),
                "Session ID (1-128 ASCII letters, digits, underscores or hyphens)")
            ("threads,t", options::value<int>(&thread_count)->default_value(1),
                "Number of io_context execution threads, including the main thread");

        options::variables_map arguments;
        options::store(options::parse_command_line(argc, argv, description), arguments);
        if (arguments.count("help")) {
            std::cout << "Usage: simplex_worker [options]\n\n" << description << '\n';
            return 0;
        }
        options::notify(arguments);
        if (thread_count < 1) {
            throw options::error("--threads must be a positive integer");
        }
        core::validate_session_id(session);
        auto config = load::read_configuration(file);

        asio::io_context context;
        core::Application app(context.get_executor(), std::move(config), session);
        // Signal delivery and signal-set cancellation share an executor even
        // when several threads are driving the context.
        auto lifecycle = asio::make_strand(context);
        asio::signal_set signals(lifecycle, SIGINT, SIGTERM);
        signals.async_wait([&](boost::system::error_code error, int) {
            if (!error) {
                app.stop();
            }
        });

        std::mutex failure_mutex;
        std::exception_ptr failure;
        // Keep the first failure, including thread creation or an unexpected
        // handler exception. Reading it is safe after every runner has joined.
        auto remember_failure = [&](std::exception_ptr error) {
            if (error) {
                std::lock_guard lock(failure_mutex);
                if (!failure) {
                    failure = error;
                }
            }
        };
        asio::co_spawn(lifecycle, app.run(), [&](std::exception_ptr error) {
            remember_failure(error);
            signals.cancel();
        });

        // A throwing handler must not terminate a background thread or abandon
        // Application's lifetime fence. Request shutdown and resume dispatching
        // its cleanup; run() may be re-entered after a handler exception.
        auto run_context = [&] {
            for (;;) {
                try {
                    context.run();
                    return;
                } catch (...) {
                    remember_failure(std::current_exception());
                    app.stop();
                }
            }
        };
        std::vector<std::jthread> runners;
        try {
            runners.reserve(static_cast<std::size_t>(thread_count - 1));
            for (int index = 1; index < thread_count; ++index) {
                runners.emplace_back(run_context);
            }
        } catch (...) {
            // Some runners may already exist. The main thread still drains
            // controlled shutdown before their destructors join them.
            remember_failure(std::current_exception());
            app.stop();
        }
        run_context();
        runners.clear();
        if (failure) {
            std::rethrow_exception(failure);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Worker: " << error.what() << '\n';
        return 1;
    }
}
