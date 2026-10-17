#define BOOST_TEST_MODULE ManagedConfiguration
#include <boost/test/unit_test.hpp>
#include <filesystem>
#include <unistd.h>
#include "load/configuration.hpp"
#include "manager/service.hpp"

/** The actual startup parser must retain the fence, including persistence=false. */
BOOST_AUTO_TEST_CASE(managed_startup_retains_allocation_until_configuration_is_released) {
    namespace fs = std::filesystem;
    using Json = nlohmann::json;
    std::string pattern = (fs::temp_directory_path() / "simplex-managed-load-XXXXXX").string();
    const char* created = ::mkdtemp(pattern.data());
    BOOST_REQUIRE(created);
    const fs::path parent(created);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); }
    } cleanup{parent};
    const auto root = parent / "worker";
    Json source = {
        {"providers", {{"local", {{"plugin", "deepseek"}, {"model", "fixture"},
            {"endpoint", {{"auth", {{"api_key", "literal"}}}}}}}}},
        {"driver_model", "local"},
        {"client", {{"endpoint", "ws://localhost:8765/events"}}},
        {"persistence", {{"enabled", false}}}
    };
    Json request = {{"version", 1}, {"request_id", "test"}, {"allocation_id", "test"},
        {"operation", "prepare"}, {"directory", root.string()}, {"configuration", source.dump()}};
    manager::execute(request);
    request["operation"] = "remove";
    {
        auto config = load::read_configuration(root / "config/config.yaml");
        BOOST_REQUIRE(config.allocation_lease);
        BOOST_TEST(config.storage == root);
        BOOST_TEST(!config.persistence);
        BOOST_TEST(config.provider == "deepseek");
        BOOST_CHECK_EXCEPTION(manager::execute(request), manager::Error,
            [](const auto& error) { return error.code == "busy"; });
        auto retained = config;
        config.allocation_lease.reset();
        BOOST_CHECK_THROW(manager::execute(request), manager::Error);
    }
    BOOST_TEST(manager::execute(request)["status"] == "removed");
    BOOST_TEST(!fs::exists(root));
}
