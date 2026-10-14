#define BOOST_TEST_MODULE CommandPolicyTests
#include <boost/test/unit_test.hpp>

#include "tools/intrinsic/process/command_policy.hpp"
#include "tools/intrinsic/process/schemas.hpp"
#include "tools/intrinsic/tool_declaration.hpp"

#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using tools::intrinsic::CommandPolicy;
using Decision = CommandPolicy::Decision;
using nlohmann::json;

namespace {

/// Exercise the installed schema's actual defaults, not a copy of its patterns.
json defaults(const char* mode)
{
    auto config = tools::intrinsic::load_tool_declaration(
        tools::intrinsic::schema_directory() / "run_command.yaml").config.at("bypass");
    config["mode"] = mode;
    return config;
}

void expect(const CommandPolicy& policy, std::string_view command, Decision expected)
{
    BOOST_TEST_CONTEXT(command) {
        BOOST_CHECK(policy.classify(command) == expected);
    }
}

} // namespace

BOOST_AUTO_TEST_CASE(packaged_readonly_rules_cover_literal_queries_and_chains)
{
    const CommandPolicy policy(defaults("readonly"));
    for (const auto* command : {
             "pwd", "pwd -P", "ls", "ls -lah ./src", "cat README.md",
             "cat -- 'a b.txt'", "head -n 20 file", "tail -c 100 file",
             "wc -l file", "sort -nu file", "uniq -c file", "grep -n needle file",
             "rg -n needle ./src", "rg 'TODO|FIXME' ./src", "grep -E '(foo|bar)' file",
             "rg '' ./src", "du -sh ./src", "id", "whoami", "uname -a", "date",
             "git --no-pager log --no-show-signature --oneline -n 20", "rg --files --hidden ./src", "stat file",
             "readlink -- link", "file --mime-type image.png",
             "git --no-pager status --short", "git --no-pager diff --no-ext-diff --no-textconv",
             "git --no-pager diff --no-ext-diff --no-textconv --stat -- src/file.cpp",
             "echo hello", "true", "false", "  ls\t-la  ",
             "cat file | sort -u | uniq -c", "pwd && ls || echo fallback"}) {
        expect(policy, command, Decision::Readonly);
    }
}

BOOST_AUTO_TEST_CASE(mode_precedence_and_default_configuration)
{
    const CommandPolicy strict(defaults("strict"));
    const CommandPolicy readonly(defaults("readonly"));
    const CommandPolicy permissive(defaults("permissive"));
    const CommandPolicy empty(json::object());
    expect(strict, "pwd", Decision::Confirm);
    expect(empty, "pwd", Decision::Confirm);
    expect(readonly, "mkdir -p ./build", Decision::Confirm);
    expect(permissive, "pwd", Decision::Readonly);
    for (const auto* command : {"mkdir build", "mkdir -p -- 'build dir'",
             "cp -n -- source destination", "ln -s -- source destination",
             "mkdir -p build && ls build", "pwd || mkdir ./new"}) {
        expect(permissive, command, Decision::TrustedWrite);
    }
    auto config = defaults("readonly");
    config["trusted_rules"] = json::array({".*"});
    expect(CommandPolicy(config), "unknown", Decision::Confirm);
    config["mode"] = "permissive";
    expect(CommandPolicy(config), "pwd", Decision::Readonly);
    config["readonly_rules"] = json::array();
    expect(CommandPolicy(config), "pwd", Decision::TrustedWrite);
    expect(CommandPolicy({{"mode", "permissive"}}), "pwd", Decision::Confirm);
}

BOOST_AUTO_TEST_CASE(dangerous_options_and_unsupported_programs_require_confirmation)
{
    const CommandPolicy policy(defaults("permissive"));
    for (const auto* command : {
             "rm file", "rmdir dir", "mv a b", "cp a b", "cp -f a b", "ln -sf a b",
             "chmod 777 file", "kill 123", "sudo ls", "env ls", "bash -c pwd",
             "sh script", "python script.py", "npm install", "make", "./ls file",
             "sort -o output input", "uniq input output", "rg --pre script needle file",
             "rg '--pre' script", "rg '--pre=script' needle", "rg --pre=script needle",
             "find . -delete", "find . -exec rm {} +", "sed -i s/a/b/ file",
             "date -s tomorrow", "du --files0-from list", "git log", "git reset --hard", "git clean -fd", "git diff", "git status",
             "git --no-pager diff --no-ext-diff --no-textconv --output file",
             "git --no-pager status --short && rm file", "cat file | sh",
             "mkdir -p build && rm file", "cat file > output", "echo hi >> file",
             "cat < file", "ls; rm file", "ls\nrm file", "ls &", "ls |& cat",
             "ls $(rm file)", "echo `id`", "echo $HOME", "echo '${HOME}'",
             "A=1 ls", "ls *", "cat ~user/file", "ls # comment", "ls \\\n",
             "(ls)", "{ ls; }", "ls ||", "&& ls", "ls && && pwd", "ls | | pwd",
             "ls 'unclosed", "ls \"unclosed", "", "  "}) {
        expect(policy, command, Decision::Confirm);
    }
    expect(policy, std::string("pwd\0;rm", 7), Decision::Confirm);
    expect(policy, std::string(4097, 'a'), Decision::Confirm);
}

BOOST_AUTO_TEST_CASE(full_matching_quote_handling_and_chain_limits)
{
    const CommandPolicy policy({{"mode", "readonly"}, {"readonly_rules", {"echo .*", "pwd"}}});
    expect(policy, "echo 'a|b && c'", Decision::Readonly);
    expect(policy, "echo \"a > b\"", Decision::Readonly);
    expect(policy, "echo a | unknown", Decision::Confirm);
    expect(policy, "pwd extra", Decision::Confirm);
    expect(policy, "otherpwd", Decision::Confirm);
    expect(policy, "PWD", Decision::Confirm);
    std::string command = "pwd";
    for (int i = 1; i < 16; ++i) {
        command += " && pwd";
    }
    expect(policy, command, Decision::Readonly);
    expect(policy, command + " && pwd", Decision::Confirm);
}

BOOST_AUTO_TEST_CASE(configuration_errors_are_rejected_without_echoing_values)
{
    for (const auto& config : std::vector<json>{
             nullptr, json::array(), {{"mode", true}}, {{"mode", "unknown"}},
             {{"unknown", 1}}, {{"readonly_rules", "pwd"}}, {{"trusted_rules", {1}}},
             {{"readonly_rules", {"[secret-invalid"}}}, {{"trusted_rules", {""}}},
             {{"trusted_rules", {std::string(1025, 'a')}}},
             {{"readonly_rules", {std::string("x\0x", 3)}}},
             {{"readonly_rules", std::vector<std::string>(33, "pwd")}}}) {
        BOOST_CHECK_EXCEPTION(CommandPolicy{config}, std::invalid_argument,
            [](const std::invalid_argument& error) {
                return std::string(error.what()).find("secret-invalid") == std::string::npos;
            });
    }
}

BOOST_AUTO_TEST_CASE(pathological_regex_fails_closed_instead_of_blocking)
{
    // A failing suffix forces exponential exploration in an unbounded matcher.
    const CommandPolicy policy({{"mode", "readonly"}, {"readonly_rules", {"(a+)+b", ".*"}}});
    expect(policy, std::string(1000, 'a') + "c", Decision::Confirm);
}

BOOST_AUTO_TEST_CASE(compiled_policy_supports_concurrent_queries)
{
    const CommandPolicy policy(defaults("permissive"));
    std::vector<std::jthread> threads;
    std::atomic<bool> correct{true};
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < 100; ++j) {
                if (policy.classify("cat file | sort -u") != Decision::Readonly ||
                    policy.classify("mkdir -p build") != Decision::TrustedWrite ||
                    policy.classify("rm file") != Decision::Confirm) {
                    correct = false;
                }
            }
        });
    }
    threads.clear();
    BOOST_TEST(correct.load());
}
