#define BOOST_TEST_MODULE LoopExtensions
#include <boost/test/unit_test.hpp>

#include "loop/extensions/plugin.hpp"
#include "loop/hook_registry.hpp"
#include "loop/events.hpp"

#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

struct Scratch {
    fs::path root = fs::temp_directory_path() / ("simplex_loop_extension_test_" + std::to_string(::getpid()));
    std::string old;
    bool had_old = false;
    Scratch() {
        if (const char* value = std::getenv("SIMPLEX_LOOP_EXTENSION_SCHEMA_DIR")) {
            old = value;
            had_old = true;
        }
        fs::remove_all(root);
        fs::create_directories(root / "noop_hook");
        BOOST_REQUIRE(::setenv("SIMPLEX_LOOP_EXTENSION_SCHEMA_DIR", root.c_str(), 1) == 0);
    }
    ~Scratch() {
        if (had_old) ::setenv("SIMPLEX_LOOP_EXTENSION_SCHEMA_DIR", old.c_str(), 1);
        else ::unsetenv("SIMPLEX_LOOP_EXTENSION_SCHEMA_DIR");
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    void write(std::string_view contents) {
        std::ofstream out(root / "noop_hook" / "config.yaml");
        out << contents;
        BOOST_REQUIRE(out.good());
    }
};

constexpr std::string_view valid_config =
    "name: noop_hook\ndescription: Diagnostic no-op hook.\nconfig: {}\n";

} // namespace

BOOST_AUTO_TEST_CASE(load_register_publish_and_release) {
    loop::extensions::LoopHookExtensionLoader loader;
    BOOST_TEST(loader.load_default() >= 1u);
    auto hook = loader.create("noop_hook");
    BOOST_REQUIRE(hook);
    eventbus::EventBus bus;
    loop::LoopHookRegistry registry(bus);
    registry.add(hook);
    BOOST_TEST(bus.subscriber_count<loop::RunStarted>() == 1u);
    BOOST_TEST(registry.contains("noop_hook"));
    model_io::AgentInputState state;
    loop::RunStarted event{state};
    bus.publish(event);
    loader = {};
    hook.reset();
    bus.publish(event);
    BOOST_TEST(registry.remove("noop_hook"));
    BOOST_TEST(registry.empty());
    BOOST_TEST(bus.subscriber_count<loop::RunStarted>() == 0u);
    bus.publish(event);
}

BOOST_AUTO_TEST_CASE(reject_bad_modules_and_duplicates) {
    for (const char* kind : {"bad_abi", "wrong_context", "missing_factory", "no_magic"}) {
        loop::extensions::LoopHookExtensionLoader loader;
        BOOST_TEST(loader.load(fs::path(LOOP_FIXTURE_ROOT) / kind) == 0u);
    }
    loop::extensions::LoopHookExtensionLoader loader;
    BOOST_TEST(loader.load("/nonexistent/simplex-loop-extensions") == 0u);
    BOOST_TEST(loader.load_default() >= 1u);
    BOOST_TEST(loader.load_default() == 0u);
}

BOOST_AUTO_TEST_CASE(config_override_and_validation) {
    Scratch scratch;
    const auto path = loop::extensions::config_file("noop_hook");
    BOOST_TEST(path == scratch.root / "noop_hook" / "config.yaml");
    BOOST_CHECK_THROW((void)loop::extensions::config_file("../escape"), std::invalid_argument);
    BOOST_CHECK_THROW((void)loop::extensions::load_config(path, "noop_hook"), std::exception);
    scratch.write(valid_config);
    BOOST_TEST(loop::extensions::load_config(path, "noop_hook").name == "noop_hook");

    loop::extensions::LoopHookExtensionLoader loader;
    BOOST_TEST(loader.load_default() >= 1u);
    BOOST_REQUIRE(loader.create("noop_hook"));
    BOOST_REQUIRE(loader.create("noop_hook", path));
    BOOST_TEST(!loader.create("noop_hook", scratch.root / "missing"));
    scratch.write("name: wrong\ndescription: No-op.\nconfig: {}\n");
    BOOST_TEST(!loader.create("noop_hook"));
    scratch.write("name: noop_hook\ndescription: No-op.\nconfig: {unexpected: true}\n");
    BOOST_TEST(!loader.create("noop_hook"));
    scratch.write("name: noop_hook\ndescription: No-op.\nconfig: {}\nextra: 1\n");
    BOOST_TEST(!loader.create("noop_hook"));
}

BOOST_AUTO_TEST_CASE(factory_failures_are_isolated) {
    for (const char* kind : {"null_product", "throw_product", "nonstandard_product", "wrong_product"}) {
        loop::extensions::LoopHookExtensionLoader loader;
        BOOST_TEST(loader.load(fs::path(LOOP_FIXTURE_ROOT) / kind) == 1u);
        BOOST_TEST(!loader.create("noop_hook"));
        BOOST_TEST(!loader.create("unknown_plugin"));
    }
}

BOOST_AUTO_TEST_CASE(directory_status_errors_are_visible) {
    Scratch scratch;
    loop::extensions::LoopHookExtensionLoader loader;
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
