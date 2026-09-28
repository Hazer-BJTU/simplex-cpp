#define BOOST_TEST_MODULE LoadSystemPrompt
#include <boost/test/unit_test.hpp>
#include "load/configuration.hpp"
#include <fstream>
#include <unistd.h>

using Json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
const char* const structured = R"(heading_level: 3
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
)";

struct Scratch {
    fs::path root = fs::temp_directory_path()
        / ("simplex_prompt_test_" + std::to_string(::getpid()));
    Scratch() { fs::create_directories(root); }
    ~Scratch() { std::error_code ignored; fs::remove_all(root, ignored); }
    /** The staged installation directory prompt lookup is anchored at. */
    fs::path installation() const { return root / "install"; }
    /** Write a file below the installation directory, creating parents. */
    fs::path install(const std::string& relative, const std::string& content) const {
        const auto path = installation() / relative;
        fs::create_directories(path.parent_path());
        std::ofstream(path) << content;
        return path;
    }
    /** Stage both shipped prompt files, which every parse loads eagerly. */
    void stage_defaults() const {
        install("prompts/coding_agent.yaml", structured);
        install("prompts/operations/compact.yaml",
            "sections:\n  - name: compact\n    text: Summarize without tools.\n");
    }
    /** Write a file beside the configuration, which prompts no longer read. */
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

BOOST_AUTO_TEST_CASE(relative_files_resolve_below_the_installation_directory) {
    Scratch scratch;
    scratch.stage_defaults();
    scratch.install("prompts/custom.yaml", structured);
    auto document = configuration();
    // One spelling per file: the config directory is not consulted, and `.` or
    // a repeated separator does not change which file that is.
    for (const auto& selected : {"prompts/custom.yaml", "./prompts//custom.yaml"}) {
        document["worker"] = {{"system_prompt_file", selected}};
        const auto parsed = load::parse_configuration(
            document, scratch.root, scratch.installation());
        BOOST_TEST(parsed.system_prompt.size() == 3u);
        BOOST_TEST(parsed.system_prompt.heading_level == 3);
        BOOST_TEST(parsed.system_prompt.render().markdown.find("### Identity") == 0u);
        BOOST_CHECK(parsed.system_prompt.find("notes")->stability == model_io::SectionStability::Growing);
        BOOST_CHECK(parsed.system_prompt.find("status")->stability == model_io::SectionStability::Volatile);
    }
    // A file that exists only beside the configuration is not a prompt source.
    scratch.write(structured);
    document["worker"] = {{"system_prompt_file", "prompt.yaml"}};
    BOOST_CHECK_THROW(load::parse_configuration(document, scratch.root, scratch.installation()),
        std::invalid_argument);
    BOOST_TEST(load::read_system_prompt(scratch.write("sections: []\n")).size() == 0u);
}

BOOST_AUTO_TEST_CASE(prompt_files_must_stay_inside_the_installation_directory) {
    Scratch scratch;
    scratch.install("prompts/custom.yaml", structured);
    auto document = configuration();
    const auto nested = scratch.installation() / "prompts" / "custom.yaml";
    std::vector<Json> invalid = {
        nested.string(), "/etc/simplex/agent.yaml", "../outside.yaml",
        "prompts/../../outside.yaml", "", std::string("bad\0path", 8),
        nullptr, 7,
        // Rooted in the OTHER path grammar. A POSIX build would read
        // `\outside.yaml` as one filename and a Windows build would resolve it
        // against the current drive's root, discarding the installation
        // directory — so the rule is spelled over the string and both readings
        // are refused, whichever platform is compiling this test.
        "\\outside.yaml", "\\rooted\\prompt.yaml", "C:\\absolute\\prompt.yaml",
        "C:prompt.yaml", "..\\outside.yaml", "prompts\\..\\..\\outside.yaml",
    };
    for (const auto& key : {"system_prompt_file", "compact_prompt_file"}) {
        for (const auto& path : invalid) {
            document["worker"] = {{key, path}};
            BOOST_CHECK_THROW(load::parse_configuration(
                document, scratch.root, scratch.installation()), std::exception);
        }
    }
}

// The other half of the portable rule: a backslash spelling is NOT traversal or
// a root, so it passes validation on every platform. On POSIX the whole string
// is then one filename, which is what this stages — the point is that the
// validation did not refuse it, not that the two platforms open the same file.
BOOST_AUTO_TEST_CASE(backslash_spellings_are_relative_paths_not_traversal) {
    Scratch scratch;
    scratch.stage_defaults();
    auto document = configuration();
    for (const auto& selected : {"prompts\\coding_agent.yaml",
                                 ".\\prompts\\coding_agent.yaml"}) {
        scratch.install(selected, structured);
        document["worker"] = {{"system_prompt_file", selected}};
        const auto parsed = load::parse_configuration(
            document, scratch.root, scratch.installation());
        BOOST_TEST(parsed.system_prompt.render().markdown.find("### Identity") == 0u);
    }
}

// Also run after relocation into stage/bin to validate release resource lookup.
BOOST_AUTO_TEST_CASE(default_prompt_is_loaded_beside_the_executable) {
    Scratch scratch;
    // Missing fields use the shipped files beside the executable, not this
    // directory and not the staged installation root.
    const auto defaults = load::parse_configuration(configuration(), scratch.root);
    BOOST_TEST(defaults.system_prompt.contains("persona"));
    BOOST_TEST(!defaults.system_prompt.render().markdown.empty());
    BOOST_TEST(defaults.compact_prompt.find("Do not call tools") != std::string::npos);
    BOOST_TEST(!fs::exists(scratch.root / "prompts/coding_agent.yaml"));
    // An explicit installation directory moves both defaults with it: nothing
    // is staged below it yet, so the executable's own prompts must not answer.
    BOOST_CHECK_THROW(load::parse_configuration(configuration(), scratch.root, scratch.installation()),
        std::invalid_argument);
    scratch.stage_defaults();
    const auto staged = load::parse_configuration(
        configuration(), scratch.root, scratch.installation());
    BOOST_TEST(staged.system_prompt.render().markdown.find("### Identity") == 0u);
    BOOST_TEST(staged.compact_prompt == "Summarize without tools.\n");
    BOOST_CHECK_THROW(load::parse_configuration(configuration(), scratch.root, "relative/install"),
        std::invalid_argument);
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
        {{"name", "memory.runtime"}}, {{"name", "signature.runtime"}}, {{"name", "environment.runtime"}}, {{"name", ""}}, {{"name", "skill.process"}}, {{"name", nullptr}},
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
    for (const auto& path : {Json("missing.yaml"), Json("prompts/missing.yaml")}) {
        document["worker"] = {{"system_prompt_file", path}};
        BOOST_CHECK_THROW(load::parse_configuration(document, scratch.root, scratch.installation()),
            std::exception);
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

BOOST_AUTO_TEST_CASE(compact_prompt_uses_installation_relative_paths_and_rejects_empty_content) {
    Scratch scratch;
    scratch.stage_defaults();
    auto document = configuration();
    scratch.install("prompts/operations/compact.yaml",
        "sections:\n  - name: compact\n    text: Summarize without tools.\n");
    document["worker"]["compact_prompt_file"] = "prompts/operations/compact.yaml";
    const auto config = load::parse_configuration(document, scratch.root, scratch.installation());
    BOOST_TEST(config.compact_prompt == "Summarize without tools.\n");
    scratch.install("prompts/operations/compact.yaml", "sections: []\n");
    BOOST_CHECK_THROW(load::parse_configuration(document, scratch.root, scratch.installation()),
        std::invalid_argument);
}
