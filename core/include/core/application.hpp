#pragma once
#include <memory>
#include <stop_token>
#include "load/configuration.hpp"
#include "llm/models.hpp"

namespace core {
/**
 * One process worker, one session and at most one active loop.
 *
 * Persistent sessions acquire exclusive local POSIX file ownership before
 * restoration; duplicate starts fail. Completion also joins process pipe I/O.
 * A running system DNS backend may delay cancellation completion (see intercom).
 * run() is single-use and is the lifetime fence for every owned task. Keep the
 * Application and executor alive until it completes. stop() is thread-safe and
 * requests controlled shutdown; it never stops the io_context. No live session
 * reference is exposed across executors. Production hosts use discovered model
 * plugins; an injected model permits offline integration tests.
 */
class Application {
public:
    /**
     * executor supplies the runtime; state is owned by a private strand.
     * configuration comes from load::read_configuration. session_id chooses
     * the snapshot directory and is validated before IO. model, when supplied,
     * bypasses driver construction but not component discovery or registries.
     */
    Application(boost::asio::any_io_executor executor, load::Configuration configuration,
                std::string session_id, std::shared_ptr<llm::LLMModel> model = {});
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    boost::asio::awaitable<void> run(std::stop_token stop = {});
    void stop();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace core
