#define BOOST_TEST_MODULE ManagedConfiguration
#include <boost/test/unit_test.hpp>
#include <filesystem>
#include <fstream>
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

/** Exercise the real manager protocol and startup parser, not just JSON rebasing.
 * Interpreter installation is shared; checkpoint/daemon directories are not. */
BOOST_AUTO_TEST_CASE(clean_fork_preserves_ptc_executable_and_isolates_state) {
    namespace fs = std::filesystem;
    using Json = nlohmann::json;
    std::string pattern = (fs::temp_directory_path() / "simplex-ptc-fork-XXXXXX").string();
    const char* created = ::mkdtemp(pattern.data());
    BOOST_REQUIRE(created);
    const fs::path temporary(created);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); }
    } cleanup{temporary};

    const auto installed = temporary / "installed/simplex-ptc";
    fs::create_directories(installed.parent_path());
    std::ofstream(installed) << "interpreter fixture";
    const std::vector<std::string> executables{
        "./bin/simplex-ptc", "bin/simplex-ptc", installed.string(), "simplex-ptc"};
    for (std::size_t i = 0; i < executables.size(); ++i) {
        const auto parent = temporary / ("parent-" + std::to_string(i));
        const auto child = temporary / ("child-" + std::to_string(i));
        Json source = {
            {"providers", {{"local", {{"plugin", "deepseek"}, {"model", "fixture"},
                {"endpoint", {{"auth", {{"api_key", "literal"}}}}}}}}},
            {"driver_model", "local"},
            {"client", {{"endpoint", "ws://localhost:8765/events"}}},
            {"persistence", {{"enabled", true}}},
            {"ptc", {{"executable", executables[i]},
                {"arguments", Json::array({"--module", "simplex_ptc.tools.files"})}}}
        };
        Json request = {{"version", 1}, {"request_id", "prepare"},
            {"allocation_id", "parent"}, {"operation", "prepare"},
            {"directory", parent.string()}, {"configuration", source.dump()}};
        manager::execute(request);
        fs::create_directories(parent / "config/bin");
        std::ofstream(parent / "config/bin/simplex-ptc") << "relative interpreter fixture";
        fs::create_directories(parent / "ptc");
        std::ofstream(parent / "ptc/checkpoint") << "parent-only interpreter state";
        request["operation"] = "capture_startup";
        request["lifecycle_id"] = "run";
        request["expected_revision"] = 1;
        const auto captured = manager::execute(request);
        Json fork = {{"version", 1}, {"request_id", "fork"}, {"operation", "clean_fork"},
            {"allocation_id", "child"}, {"directory", child.string()},
            {"source", {{"allocation_id", "parent"}, {"directory", parent.string()},
                {"lifecycle_id", "run"}, {"revision", 1}}}};
        const auto forked = manager::execute(fork);
        const auto parent_config = load::parse_configuration(
            captured.at("effective_configuration"), parent / "config");
        const auto child_config = load::parse_configuration(
            forked.at("effective_configuration"), child / "config");
        BOOST_REQUIRE(parent_config.ptc);
        BOOST_REQUIRE(child_config.ptc);
        BOOST_TEST(child_config.ptc->executable == parent_config.ptc->executable);
        BOOST_TEST(child_config.ptc->arguments == parent_config.ptc->arguments);
        if (i < 3) {
            BOOST_TEST(fs::exists(child_config.ptc->executable));
        } else {
            BOOST_TEST(child_config.ptc->executable == "simplex-ptc");
        }
        if (i >= 2) {
            BOOST_TEST(forked["effective_configuration"]["ptc"]["executable"] == executables[i]);
        }
        BOOST_TEST(parent_config.storage == parent);
        BOOST_TEST(child_config.storage == child);
        BOOST_TEST(fs::exists(parent_config.storage / "ptc/checkpoint"));
        BOOST_TEST(!fs::exists(child_config.storage / "ptc"));
    }
}
