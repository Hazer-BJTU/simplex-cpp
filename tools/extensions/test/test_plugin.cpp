#define BOOST_TEST_MODULE ToolExtensions
#include <boost/test/unit_test.hpp>

#include "tools/extensions/plugin.hpp"
#include "tools/registry.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <utility>

namespace fs = std::filesystem;

namespace {

struct Scratch {
    fs::path root = fs::temp_directory_path() / ("simplex_tool_extension_test_" + std::to_string(::getpid()));
    std::string old;
    bool had_old = false;
    Scratch() {
        if (const char* value = std::getenv("SIMPLEX_TOOL_EXTENSION_SCHEMA_DIR")) {
            old = value;
            had_old = true;
        }
        fs::remove_all(root);
        fs::create_directories(root / "noop_toolset");
        BOOST_REQUIRE(::setenv("SIMPLEX_TOOL_EXTENSION_SCHEMA_DIR", root.c_str(), 1) == 0);
    }
    ~Scratch() {
        if (had_old) ::setenv("SIMPLEX_TOOL_EXTENSION_SCHEMA_DIR", old.c_str(), 1);
        else ::unsetenv("SIMPLEX_TOOL_EXTENSION_SCHEMA_DIR");
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    void write(std::string_view file, std::string_view contents) {
        std::ofstream out(root / "noop_toolset" / file);
        out << contents;
        BOOST_REQUIRE(out.good());
    }
};

constexpr std::string_view valid_config =
    "name: noop_toolset\ndescription: Diagnostic no-op set.\nconfig: {}\n";

} // namespace

BOOST_AUTO_TEST_CASE(load_register_execute_and_pin_handle) {
    tools::extensions::ToolSetExtensionLoader loader;
    BOOST_TEST(loader.load_default() >= 1u);
    auto set = loader.create("noop_toolset");
    BOOST_REQUIRE(set);
    BOOST_TEST(set->get_tools().size() == 1u);
    BOOST_TEST(set->get_tools().front().name == "noop_probe");
    BOOST_REQUIRE(set->skill());

    tools::ToolRegistry registry;
    registry.add(set);
    model_io::InvokeQuery query;
    query.id = "probe-1";
    query.name = "noop_probe";
    boost::asio::io_context io;
    auto future = boost::asio::co_spawn(io,
        registry.execute(std::vector<model_io::InvokeQuery>{query}, io.get_executor()),
        boost::asio::use_future);
    io.run();
    auto results = future.get();
    BOOST_REQUIRE(results.size() == 1u);
    BOOST_TEST(results.front().query.id == "probe-1");
    BOOST_TEST(results.front().output.raw == "noop");
    BOOST_TEST(!tools::is_error(results.front()));

    auto handle = set->dispatch(query);
    BOOST_REQUIRE(handle);
    registry.clear();
    set.reset();
    loader = {};
    BOOST_TEST(handle->get_details().name == "noop_probe");
    io.restart();
    auto retained = boost::asio::co_spawn(
        io, handle->invoke(query), boost::asio::use_future);
    io.run();
    BOOST_TEST(retained.get().raw == "noop");
}

BOOST_AUTO_TEST_CASE(reject_bad_modules_without_poisoning_loader) {
    for (const char* kind : {"bad_abi", "wrong_context", "missing_factory", "no_magic"}) {
        tools::extensions::ToolSetExtensionLoader loader;
        BOOST_TEST(loader.load(fs::path(TOOL_FIXTURE_ROOT) / kind) == 0u);
        BOOST_TEST(loader.size() == 0u);
    }
    tools::extensions::ToolSetExtensionLoader loader;
    BOOST_TEST(loader.load("/nonexistent/simplex-tool-extensions") == 0u);
    BOOST_TEST(loader.load_default() >= 1u);
    BOOST_TEST(loader.load_default() == 0u);
}

BOOST_AUTO_TEST_CASE(config_override_and_strict_validation) {
    Scratch scratch;
    auto directory = tools::extensions::schema_directory("noop_toolset");
    BOOST_TEST(directory == scratch.root / "noop_toolset");
    BOOST_CHECK_THROW((void)tools::extensions::schema_directory("../escape"), std::invalid_argument);
    BOOST_CHECK_THROW((void)tools::extensions::load_config(directory, "noop_toolset"), std::exception);

    scratch.write("config.yaml", valid_config);
    auto config = tools::extensions::load_config(directory, "noop_toolset");
    BOOST_TEST(config.name == "noop_toolset");
    BOOST_CHECK_THROW((void)tools::extensions::load_tool(config, "../escape"), std::invalid_argument);
    BOOST_CHECK_THROW((void)tools::extensions::load_tool(config, "missing"), std::exception);

    tools::extensions::ToolSetExtensionLoader loader;
    BOOST_TEST(loader.load_default() >= 1u);
    BOOST_TEST(!loader.create("noop_toolset")); // no declaration yet
    scratch.write("noop_probe.yaml",
        "name: noop_probe\ndescription: No-op.\nargument_schema:\n  type: object\n  required: []\n  properties: {}\n");
    BOOST_REQUIRE(loader.create("noop_toolset"));
    BOOST_REQUIRE(loader.create("noop_toolset", directory));
    BOOST_TEST(!loader.create("noop_toolset", scratch.root / "missing"));
    scratch.write("config.yaml", "name: wrong\ndescription: No-op.\nconfig: {}\n");
    BOOST_TEST(!loader.create("noop_toolset"));
    scratch.write("config.yaml", "name: noop_toolset\ndescription: No-op.\nconfig: {}\nextra: 1\n");
    BOOST_TEST(!loader.create("noop_toolset"));
    scratch.write("config.yaml", "name: noop_toolset\ndescription: No-op.\nconfig: {unexpected: true}\n");
    BOOST_TEST(!loader.create("noop_toolset"));
}

BOOST_AUTO_TEST_CASE(factory_failures_are_isolated) {
    for (const char* kind : {"null_product", "throw_product", "nonstandard_product", "wrong_product"}) {
        tools::extensions::ToolSetExtensionLoader loader;
        BOOST_TEST(loader.load(fs::path(TOOL_FIXTURE_ROOT) / kind) == 1u);
        BOOST_TEST(!loader.create("noop_toolset"));
        BOOST_TEST(!loader.create("unknown_plugin"));
    }
}

BOOST_AUTO_TEST_CASE(directory_status_errors_are_visible) {
    Scratch scratch;
    tools::extensions::ToolSetExtensionLoader loader;
    BOOST_TEST(loader.load(scratch.root / "absent") == 0u);
    const auto cycle = scratch.root / "cycle";
    fs::create_symlink(cycle.filename(), cycle);
    BOOST_CHECK_EXCEPTION(
        loader.load(cycle), fs::filesystem_error,
        [&](const fs::filesystem_error& failure) {
            return failure.path1() == cycle
                && failure.code() == std::errc::too_many_symbolic_link_levels;
        });
    BOOST_TEST(loader.size() == 0u);
}

BOOST_AUTO_TEST_CASE(plugin_execute_preserves_original_ownership_across_suspension) {
    Scratch scratch;
    const auto directory = scratch.root / "ownership_probe";
    const auto marker = scratch.root / "destroyed";
    fs::create_directories(directory);
    {
        // JSON is valid YAML and safely quotes arbitrary temporary paths.
        std::ofstream config(directory / "config.yaml");
        config << nlohmann::json{
            {"name", "ownership_probe"},
            {"description", "Ownership regression fixture"},
            {"config", {{"destruction_marker", marker.string()}}}};
        BOOST_REQUIRE(config.good());
    }

    tools::extensions::ToolSetExtensionLoader loader;
    BOOST_REQUIRE(loader.load(fs::path(TOOL_FIXTURE_ROOT) / "ownership") == 1u);
    auto set = loader.create("ownership_probe", directory);
    BOOST_REQUIRE(set);
    tools::ToolRegistry registry;
    registry.add(set);
    model_io::InvokeQuery query;
    query.id = "ownership-1";
    query.name = "ownership_probe";

    boost::asio::io_context io;
    auto batch = boost::asio::co_spawn(
        io, registry.execute({query}, io.get_executor()), boost::asio::use_future);
    io.run();
    const auto results = batch.get();
    BOOST_REQUIRE(results.size() == 1u);
    BOOST_TEST(results.front().output.raw == "ownership preserved");

    // Start a second call, then discard every external owner while it is
    // suspended. The coroutine's host carrier must keep the plugin alive.
    const auto started = fs::path(marker.string() + ".started");
    BOOST_REQUIRE(fs::remove(started));
    auto handle = set->prepare(query);
    io.restart();
    auto call = boost::asio::co_spawn(
        io, set->execute(handle, query), boost::asio::use_future);
    BOOST_REQUIRE(io.poll_one() == 1u);
    BOOST_REQUIRE(fs::exists(started));
    BOOST_REQUIRE(call.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    registry.clear();
    set.reset();
    loader = {};
    handle.reset();
    BOOST_TEST(!fs::exists(marker));
    io.run();
    BOOST_TEST(call.get().output.raw == "ownership preserved");
    BOOST_TEST(fs::exists(marker));
}
