#include "subprocess.hpp"

int main() {
    boost::asio::io_context ioc;
    boost::asio::co_spawn(ioc, []() -> boost::asio::awaitable<void> {
    auto ex = co_await boost::asio::this_coro::executor;
    auto manager = std::make_shared<indextools::SubProcessManager>(ex);

    for (size_t i = 0; i < 10; ++i) {
        std::string desc = "task #" + std::to_string(i);
        std::vector<std::string> args{
            "-c",
            "time sleep " + std::to_string(i)
        };

        co_await manager->spawn(desc, "bash", std::move(args));
    }

    boost::asio::steady_timer t(co_await boost::asio::this_coro::executor);
    t.expires_after(std::chrono::milliseconds(5000));
    co_await t.async_wait(boost::asio::use_awaitable);

    auto result = co_await manager->list_status();
    std::cout << result.dump(2) << std::endl;
    }, boost::asio::detached);

    ioc.run();
    return 0;
}