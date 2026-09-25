#define BOOST_TEST_MODULE LoadSystemPrompt
#include <boost/test/unit_test.hpp>
#include "load/configuration.hpp"
#include <fstream>
#include <unistd.h>

using Json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
struct Scratch {
    fs::path root = fs::temp_directory_path()
        / ("simplex_prompt_test_" + std::to_string(::getpid()));
    Scratch() { fs::create_directories(root); }
    ~Scratch() { std::error_code ignored; fs::remove_all(root, ignored); }
    fs::path write(const std::string& content) const {
        const auto path = root / "prompt.yaml";
        std::ofstream(path) << content;
        return path;
    }
};

Json configuration() {
    return {
        {"driver_model", "fixture"},
        {"providers", {{"fixture", {{"model", "fixture"}}}}},
        {"client", {{"endpoint", "ws://localhost:8765/events"}}}
    };
}
} // namespace

BOOST_AUTO_TEST_CASE(relative_and_absolute_files_preserve_structured_sections) {
    Scratch scratch;
    const auto path = scratch.write(R"(heading_level: 3
future_field: true
sections:
  - name: persona
    title: Identity
    text: |
      First line.
      Second line.
  - name: notes
    stability: growing
    text: Notes.
  - name: status
    stability: volatile
    text: Current status.
)");
    auto document = configuration();
    for (const auto& selected : {path.filename(), path}) {
        document["worker"] = {{"system_prompt_file", selected.string()}};
        const auto parsed = load::parse_configuration(document, scratch.root);
        BOOST_TEST(parsed.system_prompt.size() == 3u);
        BOOST_TEST(parsed.system_prompt.heading_level == 3);
        BOOST_TEST(parsed.system_prompt.render().markdown.find("### Identity") == 0u);
        BOOST_CHECK(parsed.system_prompt.find("notes")->stability == model_io::SectionStability::Growing);
        BOOST_CHECK(parsed.system_prompt.find("status")->stability == model_io::SectionStability::Volatile);
    }
    // Missing field uses the shipped file beside the executable, not this directory.
    const auto defaults = load::parse_configuration(configuration(), scratch.root);
    BOOST_TEST(defaults.system_prompt.contains("persona"));
    BOOST_TEST(!defaults.system_prompt.render().markdown.empty());
    BOOST_TEST(load::read_system_prompt(scratch.write("sections: []\n")).size() == 0u);
}

BOOST_AUTO_TEST_CASE(malformed_or_missing_prompt_files_fail_with_filename_context) {
    Scratch scratch;
    const auto valid = Json{{"sections", Json::array({{
        {"name", "persona"}, {"text", "Instructions."}
    }})}};
    std::vector<Json> invalid = {
        nullptr, Json::array(), Json::object(), {{"sections", Json::object()}},
        {{"sections", Json::array({"text"})}}
    };
    for (const auto& heading : {Json(0), Json(7), Json(2.5), Json("2"), Json(nullptr)}) {
        auto document = valid;
        document["heading_level"] = heading;
        invalid.push_back(document);
    }
    for (const auto& patch : std::vector<Json>{
        {{"name", "environment.runtime"}}, {{"name", ""}}, {{"name", "skill.process"}}, {{"name", nullptr}},
        {{"text", 1}}, {{"text", nullptr}}, {{"stability", "typo"}},
        {{"stability", nullptr}}, {{"title", false}}
    }) {
        auto document = valid;
        document["sections"][0].update(patch);
        invalid.push_back(document);
    }
    auto duplicate = valid;
    duplicate["sections"].push_back(duplicate["sections"][0]);
    invalid.push_back(duplicate);
    duplicate["sections"][0]["stability"] = "volatile";
    duplicate["sections"][1]["name"] = "other";
    invalid.push_back(duplicate);
    for (const auto& document : invalid) {
        const auto path = scratch.write(document.dump());
        BOOST_CHECK_EXCEPTION(load::read_system_prompt(path), std::invalid_argument,
            [&](const auto& error) {
                return std::string(error.what()).find(path.string()) != std::string::npos;
            });
    }
    BOOST_CHECK_THROW(load::read_system_prompt(scratch.write("sections: [\n")), std::invalid_argument);
    auto document = configuration();
    for (const auto& path : {Json("missing.yaml"), Json(""), Json(nullptr), Json(7)}) {
        document["worker"] = {{"system_prompt_file", path}};
        BOOST_CHECK_THROW(load::parse_configuration(document, scratch.root), std::exception);
    }
    document["worker"] = {{"system_prompt", "legacy inline prompt"}};
    BOOST_CHECK_THROW(load::parse_configuration(document, scratch.root), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(environment_hints_resolve_paths_without_changing_the_process) {
    Scratch scratch;
    const auto cwd = fs::current_path();
    auto document = configuration();
    document["worker"]["environment"] = {
        {"workspace", "missing/../project"},
        {"platform", "Linux x86_64"},
        {"software", Json::array({"Python 3.12", "", "Docker CLI"})},
        {"future_field", true}
    };
    const auto parsed = load::parse_configuration(document, scratch.root);
    BOOST_CHECK(parsed.environment.workspace == scratch.root / "project");
    BOOST_TEST(parsed.environment.platform == "Linux x86_64");
    BOOST_TEST(parsed.environment.software.size() == 2u);
    BOOST_CHECK(fs::current_path() == cwd);
    BOOST_TEST(!fs::exists(parsed.environment.workspace));
    document["worker"]["environment"]["workspace"] = scratch.root.string();
    BOOST_CHECK(load::parse_configuration(document, scratch.root).environment.workspace == scratch.root);
    const auto empty = load::parse_configuration(configuration(), scratch.root);
    BOOST_TEST(empty.environment.workspace.empty());
    BOOST_TEST(empty.environment.platform.empty());
    BOOST_TEST(empty.environment.software.empty());
    for (const auto& invalid : std::vector<Json>{
        nullptr, Json::array(), {{"workspace", 7}}, {{"platform", false}},
        {{"workspace", std::string("bad\0path", 8)}},
        {{"software", "Python"}}, {{"software", Json::array({7})}}
    }) {
        document["worker"]["environment"] = invalid;
        BOOST_CHECK_THROW(load::parse_configuration(document, scratch.root), std::invalid_argument);
    }
}
