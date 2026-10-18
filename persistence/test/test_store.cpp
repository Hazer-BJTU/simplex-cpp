#define BOOST_TEST_MODULE PersistenceStore
#include <boost/test/unit_test.hpp>

#include "load/persistence.hpp"
#include "persistence/store.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using Json = nlohmann::json;
using persistence::ArchiveOutcome;
using persistence::ArchivePruneRequest;
using persistence::ArchiveRef;
using persistence::Backend;
using persistence::Boundary;
using persistence::CommitStatus;
using persistence::Revision;

namespace {

/** Give each test a private disk tree without modifying the working directory. */
struct Scratch {
    fs::path root = fs::temp_directory_path()
        / ("simplex_store_test_" + std::to_string(::getpid()));

    Scratch() {
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    fs::path state() const { return root / "session/state"; }
    fs::path memory() const { return root / "session/memory"; }
};

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    BOOST_REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary);
    output << text;
    BOOST_REQUIRE(output.good());
}

/** A representative state with prompt, tool, conversation and extras. */
model_io::AgentInputState example_state(std::string session = "session-1") {
    model_io::AgentInputState state;
    state.meta.session_id = std::move(session);
    state.meta.created_at = "2026-10-17T00:00:00Z";
    state.meta.updated_at = "2026-10-17T01:00:00Z";
    state.system_prompt.add_section("system", "Instructions", "Be precise.");
    model_io::Invocable tool;
    tool.name = "probe";
    tool.description = "Inspect a local value.";
    tool.argument_schema = {{"type", "object"}};
    state.tools.push_back(tool);
    model_io::UserLoopStep turn;
    turn.user_input.role = "user";
    turn.user_input.content.push_back(
        {model_io::ContentType::Text, "hello", {}, model_io::Modality::Text});
    state.turns.push_back(std::move(turn));
    state.extras = Json{{"custom", "retained"}};
    return state;
}

/** Production run identities are UUIDs; archive recognition depends on the shape. */
std::string run_identity(unsigned index) {
    char text[37]{};
    std::snprintf(text, sizeof(text), "11111111-1111-4111-8111-%012u", index);
    return text;
}

/** Compare store enums in assertions without adding stream operators to the API. */
template<typename Enum>
int code(Enum value) {
    return static_cast<int>(value);
}

/** Reserve and publish one attempt, so ordinals advance in creation order. */
ArchiveRef archive_once(
    persistence::StateStore& store, const model_io::AgentInputState& original,
    const std::string& run) {
    auto archive = store.reserve_archive({run});
    store.begin_archive(archive, original);
    return archive;
}

/**
 * Backend-neutral contract checks, reusable by a database backend.
 *
 * Only behavior promised by <persistence/store.hpp> is asserted here: absent
 * versus damaged stores, exact round trips, immutable archives, archive
 * ownership and the publication outcome contract. File layout, manifest
 * validation and retention recognition live in test_store_archive.cpp.
 */
void run_backend_contract(persistence::StateStore& store) {
    BOOST_TEST(store.session_id() == "session-1");
    BOOST_REQUIRE(!store.load_current().has_value());

    const auto original = example_state();
    const auto receipt = store.checkpoint(original, Boundary::BeforeTools);
    BOOST_TEST(code(receipt.status) == code(CommitStatus::Committed));
    auto restored = store.load_current();
    BOOST_REQUIRE(restored.has_value());
    BOOST_TEST(code(restored->backend) == code(store.backend()));
    BOOST_TEST(Json(restored->state) == Json(original));

    // A backend that reports no revision must reject an expectation instead of
    // silently ignoring it.
    if (!restored->revision) {
        BOOST_CHECK_THROW(
            store.checkpoint(original, Boundary::StepFinished, Revision{1}),
            std::invalid_argument);
    }

    // A failed attempt is settled explicitly; it keeps its archive evidence.
    const auto failed = archive_once(store, original, run_identity(0));
    store.finish_archive_attempt(failed, ArchiveOutcome::Failed);

    // A committed attempt settles itself, so no separate success step exists.
    const auto archive = archive_once(store, original, run_identity(1));
    BOOST_TEST(archive.session_id == store.session_id());
    BOOST_TEST(code(archive.backend) == code(store.backend()));
    BOOST_TEST(!archive.archive_id.empty());

    model_io::AgentInputState replacement;
    replacement.meta = original.meta;
    replacement.meta.updated_at = "2026-10-17T02:00:00Z";
    const auto committed = store.commit_compaction(archive, replacement);
    BOOST_TEST(code(committed.status) == code(CommitStatus::Committed));
    restored = store.load_current();
    BOOST_REQUIRE(restored.has_value());
    BOOST_TEST(Json(restored->state) == Json(replacement));

    ArchivePruneRequest prune;
    prune.current = archive;
    prune.policy.max_archives = 5;
    const auto cleaned = store.prune_archives(prune);
    BOOST_TEST(cleaned.removed_archives == 0u);

    auto foreign = archive;
    foreign.session_id = "another-session";
    prune.current = foreign;
    BOOST_CHECK_THROW((void)store.prune_archives(prune), std::invalid_argument);
    BOOST_CHECK_THROW(store.finish_archive_attempt(foreign, ArchiveOutcome::Failed),
        std::invalid_argument);
}

} // namespace

BOOST_AUTO_TEST_CASE(labels_match_the_worker_protocol) {
    BOOST_TEST(std::string(persistence::backend_name(Backend::Json)) == "json");
    BOOST_TEST(std::string(persistence::boundary_name(Boundary::BeforeTools)) == "before_tools");
    BOOST_TEST(std::string(persistence::boundary_name(Boundary::ResultsReady)) == "results_ready");
    BOOST_TEST(std::string(persistence::boundary_name(Boundary::StepFinished)) == "step_finished");
    BOOST_TEST(std::string(persistence::boundary_name(Boundary::RunFinished)) == "run_finished");
    BOOST_TEST(std::string(persistence::boundary_name(Boundary::Cancelled)) == "cancelled");
    BOOST_TEST(std::string(persistence::boundary_name(Boundary::Shutdown)) == "shutdown");
    BOOST_TEST(std::string(persistence::boundary_name(Boundary::Compaction)) == "compact");
    BOOST_CHECK_THROW(persistence::boundary_name(static_cast<Boundary>(99)), std::logic_error);
    BOOST_CHECK_THROW(persistence::backend_name(static_cast<Backend>(99)), std::logic_error);

    const auto stamp = persistence::utc_timestamp();
    BOOST_TEST(stamp.size() == 20u);
    BOOST_TEST(stamp[4] == '-');
    BOOST_TEST(stamp[10] == 'T');
    BOOST_TEST(stamp.back() == 'Z');
}

BOOST_AUTO_TEST_CASE(json_store_satisfies_the_backend_contract) {
    Scratch scratch;
    auto store = persistence::make_json_store("session-1", {scratch.state(), scratch.memory()});
    BOOST_TEST(code(store->backend()) == code(Backend::Json));
    run_backend_contract(*store);
}

BOOST_AUTO_TEST_CASE(absent_store_is_empty_and_reading_creates_nothing) {
    Scratch scratch;
    auto store = persistence::make_json_store("session-1", {scratch.state(), scratch.memory()});
    BOOST_TEST(!store->load_current().has_value());
    BOOST_TEST(!fs::exists(scratch.state()));
    BOOST_TEST(!fs::exists(scratch.memory()));
}

BOOST_AUTO_TEST_CASE(checkpoint_matches_the_compatibility_file_api) {
    Scratch scratch;
    auto store = persistence::make_json_store("session-1", {scratch.state(), scratch.memory()});
    const auto state = example_state();
    const auto receipt = store->checkpoint(state, Boundary::StepFinished);
    BOOST_TEST(code(receipt.backend) == code(Backend::Json));
    BOOST_TEST(!receipt.revision.has_value());
    load::save_state(scratch.root / "reference.json", state);
    BOOST_TEST(read_file(scratch.state() / "state.json")
        == read_file(scratch.root / "reference.json"));
    const auto restored = store->load_current();
    BOOST_REQUIRE(restored.has_value());
    BOOST_TEST(Json(restored->state) == Json(state));
}

BOOST_AUTO_TEST_CASE(damaged_or_foreign_state_is_never_an_empty_session) {
    Scratch scratch;
    fs::create_directories(scratch.state());
    auto store = persistence::make_json_store("session-1", {scratch.state(), scratch.memory()});
    for (const auto* content : {"{\"meta\": ", "# not a snapshot\n"}) {
        write_file(scratch.state() / "state.json", content);
        BOOST_CHECK_EXCEPTION(store->load_current(), persistence::StoreError,
            [](const auto& error) {
                return error.backend() == Backend::Json
                    && error.status() == CommitStatus::NotCommitted;
            });
    }
}

BOOST_AUTO_TEST_CASE(archive_naming_never_reuses_a_reserved_ordinal) {
    Scratch scratch;
    auto store = persistence::make_json_store("session-1", {scratch.state(), scratch.memory()});
    const auto original = example_state();
    std::vector<ArchiveRef> archives;
    for (int index = 0; index < 3; ++index) {
        archives.push_back(archive_once(*store, original, run_identity(index + 1)));
    }
    for (std::size_t index = 0; index < archives.size(); ++index) {
        const auto& archive = archives[index];
        auto ordinal = std::to_string(index + 1);
        ordinal.insert(0, 20 - ordinal.size(), '0');
        BOOST_TEST(code(archive.backend) == code(Backend::Json));
        BOOST_TEST(code(archive.kind) == code(persistence::ArchiveKind::JsonState));
        BOOST_TEST(archive.restorable());
        BOOST_TEST(archive.payload.parent_path().parent_path() == scratch.memory());
        BOOST_TEST(archive.payload.filename().string() == "state.json");
        BOOST_TEST(archive.archive_id.starts_with(ordinal + "-"));
        BOOST_TEST(!read_file(archive.payload).empty());
        BOOST_TEST(fs::exists(archive.payload.parent_path() / "manifest.json"));
    }
    // The next reservation is new: archives are never reused or overwritten.
    const auto next = store->reserve_archive({run_identity(4)});
    BOOST_TEST(next.archive_id.starts_with("00000000000000000004-"));
    BOOST_TEST(!fs::exists(next.payload));
}

BOOST_AUTO_TEST_CASE(begin_archive_refuses_an_existing_attempt) {
    Scratch scratch;
    auto store = persistence::make_json_store("session-1", {scratch.state(), scratch.memory()});
    const auto original = example_state();
    const auto archive = archive_once(*store, original, run_identity(1));
    BOOST_CHECK_EXCEPTION(store->begin_archive(archive, original), persistence::StoreError,
        [](const auto& error) {
            return error.status() == CommitStatus::NotCommitted
                && std::string(error.what()).find("already exists") != std::string::npos;
        });
    // The frozen evidence of the first attempt is unchanged.
    BOOST_TEST(!read_file(archive.payload).empty());
}

BOOST_AUTO_TEST_CASE(archive_payload_is_frozen_while_the_replacement_commits) {
    Scratch scratch;
    auto store = persistence::make_json_store("session-1", {scratch.state(), scratch.memory()});
    const auto original = example_state();
    const auto archive = archive_once(*store, original, run_identity(1));
    const auto manifest = archive.payload.parent_path() / "manifest.json";
    const auto frozen = read_file(archive.payload);
    const auto frozen_manifest = read_file(manifest);
    BOOST_TEST(frozen.find("hello") != std::string::npos);
    BOOST_TEST(frozen.find("COMPACT") == std::string::npos);

    auto replacement = original;
    replacement.turns.clear();
    store->commit_compaction(archive, replacement);
    BOOST_TEST(read_file(archive.payload) == frozen);
    BOOST_TEST(read_file(manifest) == frozen_manifest);
    const auto restored = store->load_current();
    BOOST_REQUIRE(restored.has_value());
    BOOST_TEST(restored->state.turns.empty());
}

BOOST_AUTO_TEST_CASE(filesystem_failures_surface_as_store_errors) {
    Scratch scratch;
    fs::create_directories(scratch.root / "session");
    // An archive root that is a regular file cannot be scanned or populated.
    write_file(scratch.memory(), "not a directory");
    auto store = persistence::make_json_store("session-1", {scratch.state(), scratch.memory()});
    BOOST_CHECK_EXCEPTION(
        store->reserve_archive({run_identity(1)}), persistence::StoreError,
        [](const auto& error) {
            return code(error.backend()) == code(Backend::Json)
                && code(error.status()) == code(CommitStatus::NotCommitted);
        });
    const auto identity = "00000000000000000001-2026-10-17T000000Z-" + run_identity(1);
    const auto archive = ArchiveRef{"session-1", identity, Backend::Json,
        persistence::ArchiveKind::JsonState, scratch.memory() / identity / "state.json"};
    BOOST_CHECK_EXCEPTION(
        store->begin_archive(archive, example_state()), persistence::StoreError,
        [](const auto& error) {
            return code(error.status()) == code(CommitStatus::NotCommitted);
        });
}

BOOST_AUTO_TEST_CASE(archive_references_reject_a_mismatched_payload) {
    Scratch scratch;
    auto store = persistence::make_json_store("session-1", {scratch.state(), scratch.memory()});
    const auto original = example_state();
    const auto archive = archive_once(*store, original, run_identity(1));
    auto mismatched = archive;
    mismatched.payload = archive.payload.parent_path() / "state.md";
    ArchivePruneRequest prune;
    prune.current = mismatched;
    BOOST_CHECK_THROW((void)store->prune_archives(prune), std::invalid_argument);
    BOOST_CHECK_THROW(
        store->finish_archive_attempt(mismatched, ArchiveOutcome::Failed),
        std::invalid_argument);
}
