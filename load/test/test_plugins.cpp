#define BOOST_TEST_MODULE LoadPlugins
#include <boost/test/unit_test.hpp>

#include "load/plugins.hpp"
#include "loop/events.hpp"
#include "loop/hook_registry.hpp"
#include "tools/registry.hpp"
#include "yamlconfig/yaml_json.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

/** Isolated configuration and discovery tree, removed even after a test fails. */
struct Scratch {
    fs::path root = fs::temp_directory_path()
        / ("simplex_load_test_" + std::to_string(::getpid()));

    Scratch() {
        fs::remove_all(root);
        fs::create_directories(root);
    }

    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    void write(const fs::path& relative, std::string_view content) {
        fs::create_directories((root / relative).parent_path());
        std::ofstream output(root / relative);
        output << content;
        BOOST_REQUIRE(output.good());
    }

    void module(const fs::path& source, const fs::path& directory) {
        fs::create_directories(root / directory);
        fs::create_symlink(source, root / directory / source.filename());
    }

    void hook(std::string_view name) {
        write(std::string(name) + ".yaml",
              "name: " + std::string(name) + "\ndescription: Test hook.\nconfig: {}\n");
    }
};

/** Construct one domain section while leaving the rest of startup unspecified. */
Json selected(const char* domain, Json entries) {
    return {{"plugins", {{"extensions", {{domain, {{"enable", std::move(entries)}}}}}}}};
}

/** Check diagnostics include an actionable field path without dumping values. */
bool mentions(const load::PluginLoadError& error, std::string_view text) {
    return std::string_view(error.what()).find(text) != std::string_view::npos;
}

} // namespace

BOOST_AUTO_TEST_CASE(provider_discovery_ignores_model_selection_and_credentials) {
    Scratch scratch;
    Json configuration = {
        {"providers", {{"unused", {{"plugin", "absent"}, {"credential", "${UNSET}"}}}}},
        {"driver_model", "not-instantiated"},
        {"future_field", true}
    };
    auto providers = load::load_providers(configuration, scratch.root);
    BOOST_TEST(providers.usable_count() == 2u);
    BOOST_TEST(providers.contains("deepseek"));
    BOOST_TEST(providers.contains("openai"));
    boost::asio::io_context io;
    BOOST_CHECK(providers.create_model(
        "deepseek", io.get_executor(), {{"model", "test-model"}}));
    BOOST_CHECK(providers.create_model(
        "openai", io.get_executor(), {{"model", "test-model"}}));
}

BOOST_AUTO_TEST_CASE(provider_directories_merge_with_correct_routable_count) {
    Scratch scratch;
    scratch.module(LOAD_OPENAI_MODULE, "a");
    scratch.module(LOAD_DEEPSEEK_MODULE, "b");
    // Foreign-domain and malformed modules must not hide compatible providers.
    scratch.module(LOAD_TOOL_MODULE, "a");
    scratch.write("a/broken.so", "not a native module");
    auto configuration = Json{
        {"plugins", {{"providers", {{"directories", {"a", "missing", "b", "a"}}}}}}
    };
    auto providers = load::load_providers(configuration, scratch.root);
    BOOST_TEST(providers.usable_count() == 2u);
    BOOST_TEST(providers.contains("openai"));
    BOOST_TEST(providers.contains("deepseek"));
    // Re-importing must not inflate the number of routable names either.
    BOOST_TEST(providers.load_models(scratch.root / "a") == 2u);

    configuration["plugins"]["providers"]["directories"] = {"missing"};
    BOOST_TEST(load::load_providers(configuration, scratch.root).empty());
}

BOOST_AUTO_TEST_CASE(empty_enable_lists_do_not_scan_extension_directories) {
    Scratch scratch;
    fs::create_symlink("cycle", scratch.root / "cycle");
    Json configuration = {{"plugins", {{"extensions", {
        {"tools", {{"directories", {"cycle"}}}},
        {"loop_hooks", {{"directories", {"cycle"}}, {"enable", Json::array()}}}
    }}}}};
    auto extensions = load::load_extensions(configuration, scratch.root);
    BOOST_TEST(extensions.tools.empty());
    BOOST_TEST(extensions.loop_hooks.empty());
}

BOOST_AUTO_TEST_CASE(selected_default_extensions_survive_loader_and_enter_registries) {
    Scratch scratch;
    auto configuration = selected("tools", {{{"name", "noop_toolset"}}});
    configuration["plugins"]["extensions"]["loop_hooks"]["enable"] =
        Json::array({{{"name", "noop_hook"}}});
    auto extensions = load::load_extensions(configuration, scratch.root);
    BOOST_REQUIRE_EQUAL(extensions.tools.size(), 1u);
    BOOST_REQUIRE_EQUAL(extensions.loop_hooks.size(), 1u);
    eventbus::EventBus bus;
    BOOST_TEST(bus.subscriber_count<loop::RunStarted>() == 0u);
    loop::LoopHookRegistry hooks(bus);
    hooks.add(extensions.loop_hooks.front());
    tools::ToolRegistry tools;
    tools.add(extensions.tools.front());
    extensions = {};
    BOOST_TEST(bus.subscriber_count<loop::RunStarted>() == 1u);
    model_io::AgentInputState state;
    bus.publish(loop::RunStarted{state});

    model_io::InvokeQuery query;
    query.id = "probe";
    query.name = "noop_probe";
    boost::asio::io_context io;
    auto completion = boost::asio::co_spawn(
        io, tools.execute({query}, io.get_executor()), boost::asio::use_future);
    io.run();
    const auto results = completion.get();
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_TEST(results.front().output.raw == "noop");
}

BOOST_AUTO_TEST_CASE(relative_component_paths_and_hook_order_are_preserved) {
    Scratch scratch;
    scratch.module(LOAD_TOOL_MODULE, "modules");
    fs::copy(LOAD_TOOL_SCHEMA, scratch.root / "schema", fs::copy_options::recursive);
    scratch.hook("first");
    scratch.hook("second");
    scratch.hook("unselected");
    auto configuration = selected("tools", Json::array({{
        {"name", "noop_toolset"}, {"schema_directory", "schema"}
    }}));
    configuration["plugins"]["extensions"]["tools"]["directories"] = {"modules"};
    configuration["plugins"]["extensions"]["loop_hooks"] = {
        {"directories", {LOAD_FIXTURE_DIRECTORY}},
        {"enable", {
            {{"name", "second"}, {"config_file", "second.yaml"}},
            {{"name", "first"}, {"config_file", "first.yaml"}}
        }}
    };
    const auto loaded = load::load_extensions(configuration, scratch.root);
    BOOST_REQUIRE_EQUAL(loaded.tools.size(), 1u);
    BOOST_REQUIRE_EQUAL(loaded.loop_hooks.size(), 2u);
    BOOST_TEST(loaded.loop_hooks[0]->name() == "second");
    BOOST_TEST(loaded.loop_hooks[1]->name() == "first");

    // The failing factory is harmless until explicitly selected.
    configuration["plugins"]["extensions"]["loop_hooks"]["enable"].push_back({
        {"name", "unselected"}, {"config_file", "unselected.yaml"}
    });
    BOOST_CHECK_EXCEPTION((void)load::load_extensions(configuration, scratch.root),
        load::PluginLoadError, [](const auto& error) {
            return mentions(error, "/plugins/extensions/loop_hooks/enable/2");
        });
}

BOOST_AUTO_TEST_CASE(missing_or_bad_selected_configuration_fails_without_fallback) {
    Scratch scratch;
    for (const char* domain : {"tools", "loop_hooks"}) {
        auto configuration = selected(domain, {{{"name", "missing"}}});
        BOOST_CHECK_THROW((void)load::load_extensions(configuration, scratch.root), load::PluginLoadError);
        const bool tool = std::string_view(domain) == "tools";
        configuration = selected(domain, Json::array({{
            {"name", tool ? "noop_toolset" : "noop_hook"},
            {tool ? "schema_directory" : "config_file", "missing"}
        }}));
        BOOST_CHECK_THROW((void)load::load_extensions(configuration, scratch.root), load::PluginLoadError);
    }
    scratch.write("bad.yaml", "name: noop_hook\ndescription: Test.\nconfig: {unexpected: true}\n");
    const auto configuration = selected("loop_hooks", Json::array({{
        {"name", "noop_hook"}, {"config_file", "bad.yaml"}
    }}));
    BOOST_CHECK_THROW((void)load::load_extensions(configuration, scratch.root), load::PluginLoadError);
}

BOOST_AUTO_TEST_CASE(invalid_known_fields_are_rejected_and_unknown_fields_ignored) {
    Scratch scratch;
    const std::vector<Json> invalid_configurations = {
        nullptr, Json::array(), {{"plugins", nullptr}},
        {{"plugins", {{"extensions", 1}}}},
        {{"plugins", {{"extensions", {{"tools", nullptr}}}}}},
        selected("tools", "noop_toolset"),
        selected("tools", Json::array({"noop_toolset"})),
        selected("tools", Json::array({Json::object()})),
        selected("tools", {{{"name", ""}}}),
        selected("tools", {{{"name", "../escape"}}}),
        selected("tools", {{{"name", "noop_toolset"}, {"schema_directory", ""}}}),
        selected("loop_hooks", {{{"name", "noop_hook"}, {"config_file", nullptr}}}),
        selected("tools", {{{"name", "noop_toolset"}}, {{"name", "noop_toolset"}}})
    };
    for (const auto& configuration : invalid_configurations) {
        BOOST_CHECK_THROW((void)load::load_extensions(configuration, scratch.root), load::PluginLoadError);
    }
    for (const auto& value : std::vector<Json>{nullptr, 3, "path", Json::array({""})}) {
        const Json configuration = {{"plugins", {{"providers", {{"directories", value}}}}}};
        BOOST_CHECK_THROW((void)load::load_providers(configuration, scratch.root), load::PluginLoadError);
    }
    auto configuration = selected("tools", {{{"name", "noop_toolset"}, {"future", true}}});
    configuration["plugins"]["future"] = Json::array();
    BOOST_TEST(load::load_extensions(configuration, scratch.root).tools.size() == 1u);
    BOOST_CHECK_THROW((void)load::load_extensions(Json::object(), "relative"), load::PluginLoadError);
}

BOOST_AUTO_TEST_CASE(both_lists_are_validated_before_any_directory_is_opened) {
    Scratch scratch;
    fs::create_symlink("cycle", scratch.root / "cycle");
    auto configuration = selected("tools", {{{"name", "noop_toolset"}}});
    configuration["plugins"]["extensions"]["tools"]["directories"] = {"cycle"};
    configuration["plugins"]["extensions"]["loop_hooks"]["enable"] = false;
    BOOST_CHECK_EXCEPTION((void)load::load_extensions(configuration, scratch.root),
        load::PluginLoadError, [](const auto& error) {
            return mentions(error, "/plugins/extensions/loop_hooks/enable");
        });
    configuration["plugins"]["extensions"].erase("loop_hooks");
    BOOST_CHECK_THROW((void)load::load_extensions(configuration, scratch.root), fs::filesystem_error);
}

BOOST_AUTO_TEST_CASE(yaml_entrypoint_uses_file_parent_and_reports_source) {
    Scratch scratch;
    scratch.module(LOAD_OPENAI_MODULE, "modules");
    scratch.write("startup.yaml",
        "plugins:\n  providers:\n    directories: [modules]\n"
        "future_field: true\nproviders: {unused: {credential: '${NOT_READ}'}}\n");
    auto loaded = load::load_plugins(scratch.root / "startup.yaml");
    BOOST_TEST(loaded.providers.usable_count() == 1u);
    BOOST_TEST(loaded.providers.contains("openai"));
    BOOST_TEST(loaded.extensions.tools.empty());
    scratch.write("invalid.yaml", "plugins: {extensions: {tools: {enable: false}}}\n");
    BOOST_CHECK_EXCEPTION((void)load::load_plugins(scratch.root / "invalid.yaml"),
        load::PluginLoadError, [](const auto& error) {
            return mentions(error, "invalid.yaml") && mentions(error, "/plugins/extensions/tools/enable");
        });
    scratch.write("syntax.yaml", "plugins: [\n");
    BOOST_CHECK_THROW((void)load::load_plugins(scratch.root / "syntax.yaml"), load::PluginLoadError);
    BOOST_CHECK_THROW((void)load::load_plugins(scratch.root / "missing.yaml"), load::PluginLoadError);
}

BOOST_AUTO_TEST_CASE(shipped_template_loads_plugins_without_credentials_or_network) {
    const auto loaded = load::load_plugins(LOAD_TEMPLATE);
    BOOST_TEST(loaded.providers.usable_count() == 2u);
    BOOST_TEST(loaded.extensions.tools.empty());
    BOOST_TEST(loaded.extensions.loop_hooks.empty());
}
