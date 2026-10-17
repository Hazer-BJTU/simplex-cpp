#define BOOST_TEST_MODULE manager
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include "manager/service.hpp"
#include "load/persistence.hpp"

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;

/** Each test owns a fresh target root and its stable sibling control directory. */
struct Fixture {
    fs::path temporary;
    fs::path root;
    Json request;
    Fixture() {
        std::string pattern = (fs::temp_directory_path() / "simplex-manager-XXXXXX").string();
        const auto* created = ::mkdtemp(pattern.data());
        if (!created) {
            throw std::runtime_error("mkdtemp failed");
        }
        temporary = created;
        root = temporary / "target allocation";
        request = {{"version", 1}, {"request_id", "request-1"},
            {"allocation_id", "allocation-1"}, {"directory", root.string()},
            {"operation", "prepare"},
            {"configuration", "# Keep this comment and Unicode: 中文\npersistence:\n  state: state\n  memory: memory\nclient:\n  endpoint: ws://old/\nunknown:\n  nested: [1, two]\n"}};
    }
    ~Fixture() { fs::remove_all(temporary); }
    Json call(const std::string& op) {
        auto query = request;
        query["operation"] = op;
        return manager::execute(query);
    }
    Json reference() const {
        std::ifstream input(root / "config/config.yaml");
        return Json::parse(input);
    }
};

bool code(const manager::Error& error, const std::string& expected) {
    return error.code == expected;
}
}

BOOST_FIXTURE_TEST_CASE(prepare_preserves_source_and_refreshes_only_owned_fields, Fixture) {
    const auto prepared = call("prepare");
    BOOST_TEST(prepared.at("configuration") == request.at("configuration"));
    BOOST_TEST(prepared["effective_configuration"]["persistence"]["directory"] == root.string());
    BOOST_TEST(call("prepare") == prepared);
    request["refresh"] = {{"client_endpoint", "ws://new/"}};
    BOOST_CHECK_EXCEPTION(call("prepare"), manager::Error,
        [](const auto& error) { return code(error, "conflict"); });
    request["expected_revision"] = 1;
    const auto refreshed = call("prepare");
    BOOST_TEST(refreshed["revision"] == 2);
    BOOST_TEST(refreshed["configuration"] == request["configuration"]);
    BOOST_TEST(refreshed["effective_configuration"]["client"]["endpoint"] == "ws://new/");
    BOOST_TEST(refreshed["effective_configuration"]["unknown"]["nested"].size() == 2);
    BOOST_TEST(!refreshed["effective_configuration"].contains("hub_remote_call"));
    BOOST_TEST(call("prepare") == refreshed); // Lost-response retry is harmless.
}

BOOST_FIXTURE_TEST_CASE(worker_lease_fences_mutations_but_allows_readers, Fixture) {
    call("prepare");
    auto worker = manager::resolve_configuration(root / "config/config.yaml", reference());
    BOOST_REQUIRE(worker.has_value());
    BOOST_TEST(worker->document["persistence"]["directory"] == root.string());
    BOOST_TEST(call("inspect")["status"] == "ready");
    BOOST_TEST(call("read_state")["state"].is_null());
    BOOST_CHECK_EXCEPTION(call("remove"), manager::Error,
        [](const auto& error) { return code(error, "busy"); });
    BOOST_CHECK_EXCEPTION(call("prepare"), manager::Error,
        [](const auto& error) { return code(error, "busy"); });
    worker.reset();
    BOOST_TEST(call("remove")["status"] == "removed");
    BOOST_TEST(!fs::exists(root));
    BOOST_TEST(call("remove")["status"] == "removed");
    BOOST_CHECK_EXCEPTION(manager::resolve_configuration(root / "config/config.yaml",
        {{"_simplex_managed", {{"version", 1}, {"directory", root.string()}, {"allocation_id", "allocation-1"}}}}),
        manager::Error, [](const auto& error) { return code(error, "not_ready"); });
    BOOST_TEST(!fs::exists(root)); // A delayed worker must never recreate it.
    BOOST_CHECK_EXCEPTION(call("prepare"), manager::Error,
        [](const auto& error) { return code(error, "retired"); });
}

BOOST_FIXTURE_TEST_CASE(legacy_lock_wrong_owner_and_unmanaged_directories_are_protected, Fixture) {
    fs::create_directory(root);
    BOOST_CHECK_EXCEPTION(call("prepare"), manager::Error,
        [](const auto& error) { return code(error, "unmanaged_directory"); });
    fs::remove(root);
    call("prepare");
    request["allocation_id"] = "other";
    BOOST_CHECK_EXCEPTION(call("remove"), manager::Error,
        [](const auto& error) { return code(error, "ownership_mismatch"); });
    request["allocation_id"] = "allocation-1";
    {
        fileio::SessionLock legacy(root / "session.lock");
        BOOST_CHECK_EXCEPTION(call("remove"), manager::Error,
            [](const auto& error) { return code(error, "busy"); });
    }
    BOOST_TEST(fs::exists(root / "config/config.yaml"));
    call("remove");
}

BOOST_FIXTURE_TEST_CASE(snapshot_is_complete_and_corrupt_data_does_not_become_empty_state, Fixture) {
    call("prepare");
    model_io::AgentInputState state;
    state.extras = Json{{"large", std::string(10 * 1024 * 1024, 'x')}};
    const auto path = root / "state/state.json";
    load::save_state(path, state);
    const auto result = call("read_state");
    BOOST_TEST(result["state"]["extras"]["large"].get<std::string>().size() == 10 * 1024 * 1024);
    BOOST_TEST(result["state"] == Json(state));
    std::ofstream(path) << "{\"broken\":";
    BOOST_CHECK_THROW(call("read_state"), manager::Error);
    std::ofstream(path) << "{}";
    BOOST_CHECK_THROW(call("read_state"), std::exception);
}

BOOST_FIXTURE_TEST_CASE(startup_capture_is_revision_checked_and_lifecycle_scoped, Fixture) {
    call("prepare");
    request["lifecycle_id"] = "run-1";
    request["expected_revision"] = 1;
    const auto captured = call("capture_startup");
    BOOST_TEST(captured["startup"]["configuration"] == captured["effective_configuration"]);
    BOOST_TEST(call("capture_startup") == captured);
    request["refresh"] = {{"client_endpoint", "ws://later/"}};
    call("prepare");
    request["expected_revision"] = 2;
    BOOST_CHECK_EXCEPTION(call("capture_startup"), manager::Error,
        [](const auto& error) { return code(error, "conflict"); });
    request["lifecycle_id"] = "run-2";
    BOOST_TEST(call("capture_startup")["startup"]["lifecycle_id"] == "run-2");
}

BOOST_FIXTURE_TEST_CASE(symlinks_and_moved_references_are_rejected, Fixture) {
    call("prepare");
    BOOST_CHECK_THROW(manager::resolve_configuration(temporary / "copied.yaml", reference()), manager::Error);
    fs::create_directory(temporary / "elsewhere");
    fs::create_directory_symlink(temporary / "elsewhere", root / "state");
    BOOST_CHECK_THROW(call("read_state"), manager::Error);
    // Interior links are unlinked, never traversed, during owned allocation cleanup.
    std::ofstream(temporary / "elsewhere/keep") << "keep";
    call("remove");
    BOOST_TEST(fs::exists(temporary / "elsewhere/keep"));
}

BOOST_FIXTURE_TEST_CASE(interrupted_prepare_and_remove_are_reconcilable, Fixture) {
    call("prepare");
    const auto control = temporary / ".simplex-manager/target allocation.json";
    Json record;
    { std::ifstream input(control); input >> record; }
    record["status"] = "preparing";
    fs::remove_all(root);
    std::ofstream(control) << record;
    BOOST_TEST(call("prepare")["status"] == "ready");
    record["status"] = "removing";
    std::ofstream(control) << record;
    fs::remove_all(root); // Simulate exit after deletion but before tombstone.
    BOOST_TEST(call("remove")["status"] == "removed");
}

BOOST_FIXTURE_TEST_CASE(unsupported_backend_and_invalid_input_do_not_mutate_storage, Fixture) {
    request["configuration"] = "persistence: {state: ../outside}";
    BOOST_CHECK_THROW(call("prepare"), manager::Error);
    BOOST_TEST(!fs::exists(root));
    request["configuration"] = "persistence: {backend: sqlite}";
    call("prepare");
    BOOST_CHECK_EXCEPTION(call("read_state"), manager::Error,
        [](const auto& error) { return code(error, "unsupported_backend"); });
    BOOST_TEST(!manager::resolve_configuration(temporary / "ordinary.yaml", Json::object()).has_value());
}

BOOST_FIXTURE_TEST_CASE(refresh_preserves_permissions_and_failed_refresh_is_atomic, Fixture) {
    call("prepare");
    const auto control = temporary / ".simplex-manager/target allocation.json";
    fs::permissions(control, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
    const auto before = call("inspect");
    request["expected_revision"] = 1;
    request["refresh"] = {{"unknown_field", "secret-value"}};
    BOOST_CHECK_THROW(call("prepare"), manager::Error);
    BOOST_TEST(call("inspect") == before);
    request["refresh"] = {{"client_endpoint", "ws://changed/"}};
    call("prepare");
    BOOST_CHECK(fs::status(control).permissions()
        == (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read));
}

BOOST_FIXTURE_TEST_CASE(independent_allocations_cannot_be_nested, Fixture) {
    call("prepare");
    request["directory"] = (root / "child").string();
    request["allocation_id"] = "child";
    BOOST_CHECK_EXCEPTION(call("prepare"), manager::Error,
        [](const auto& error) { return code(error, "unsafe_path"); });
    BOOST_TEST(!fs::exists(root / "child"));
    request["directory"] = (temporary / ".simplex-manager/child").string();
    BOOST_CHECK_THROW(call("prepare"), manager::Error);
}

BOOST_FIXTURE_TEST_CASE(clean_fork_uses_captured_target_configuration_and_no_state, Fixture) {
    request["configuration"] = R"({"persistence":{"restore":"required","state":"state","memory":"memory"},
        "worker":{"environment":{"workspace":"../../work"},"system_prompt_file":"prompts/custom.yaml"},
        "plugins":{"providers":{"directories":["../../providers"]}},
        "providers":{"test":{"model":"captured"}},"unknown":{"preserved":true}})";
    call("prepare");
    request["lifecycle_id"] = "parent-run";
    request["expected_revision"] = 1;
    call("capture_startup");
    fs::create_directories(root / "state");
    std::ofstream(root / "state/state.json") << "private parent history";
    fs::create_directories(root / "memory");
    std::ofstream(root / "memory/archive.md") << "private parent archive";
    auto worker = manager::resolve_configuration(root / "config/config.yaml", reference());
    const auto child_root = temporary / "child";
    Json fork = {{"version", 1}, {"request_id", "fork-1"}, {"operation", "clean_fork"},
        {"allocation_id", "child-1"}, {"directory", child_root.string()},
        {"source", {{"allocation_id", "allocation-1"}, {"directory", root.string()},
            {"lifecycle_id", "parent-run"}, {"revision", 1}}}};
    const auto child = manager::execute(fork); // Shared parent lease permits a running parent.
    const auto& config = child.at("effective_configuration");
    BOOST_TEST(config["providers"]["test"]["model"] == "captured");
    BOOST_TEST(config["unknown"]["preserved"] == true);
    BOOST_TEST(config["persistence"]["directory"] == child_root.string());
    BOOST_TEST(config["persistence"]["restore"] == "if_present");
    BOOST_TEST(config["client"]["endpoint"] == "ws://unconfigured.invalid/events");
    BOOST_TEST(config["worker"]["environment"]["workspace"] == (temporary / "work").string());
    BOOST_TEST(config["worker"]["system_prompt_file"] == "prompts/custom.yaml");
    BOOST_TEST(config["plugins"]["providers"]["directories"][0] == (temporary / "providers").string());
    BOOST_TEST(!fs::exists(child_root / "state"));
    BOOST_TEST(!fs::exists(child_root / "memory"));
    BOOST_TEST(manager::execute(fork) == child);
    // Simulate interruption after the ownership/source record was published
    // but before child files and the ready status became durable.
    const auto child_record_path = temporary / ".simplex-manager/child.json";
    Json interrupted;
    { std::ifstream input(child_record_path); input >> interrupted; }
    interrupted["status"] = "preparing";
    fs::remove_all(child_root);
    { std::ofstream output(child_record_path); output << interrupted; }
    BOOST_TEST(manager::execute(fork) == child);
    auto conflict = fork;
    conflict["source"]["lifecycle_id"] = "another-run";
    BOOST_CHECK_EXCEPTION(manager::execute(conflict), manager::Error,
        [](const auto& error) { return code(error, "conflict"); });
    conflict["directory"] = (temporary / "other-child").string();
    BOOST_CHECK_EXCEPTION(manager::execute(conflict), manager::Error,
        [](const auto& error) { return code(error, "conflict"); });
    BOOST_TEST(!fs::exists(temporary / "other-child"));
    worker.reset();
    call("remove");
    BOOST_TEST(manager::execute(fork) == child); // Lost reply retry does not need live parent.
    auto remove = fork;
    remove["operation"] = "remove";
    manager::execute(remove);
    BOOST_CHECK_EXCEPTION(manager::execute(fork), manager::Error,
        [](const auto& error) { return code(error, "retired"); });
}
