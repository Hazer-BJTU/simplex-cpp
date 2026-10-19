#define BOOST_TEST_MODULE PersistenceStoreArchive
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

namespace fs = std::filesystem;
using Json = nlohmann::json;
using persistence::ArchiveKind;
using persistence::ArchivePruneRequest;
using persistence::ArchiveRef;
using persistence::Backend;

namespace {

/** Give each test a private disk tree without modifying the working directory. */
struct Scratch {
    fs::path root = fs::temp_directory_path()
        / ("simplex_store_archive_" + std::to_string(::getpid()));

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

Json read_json(const fs::path& path) {
    return Json::parse(read_file(path));
}

void write_json(const fs::path& path, const Json& document) {
    write_file(path, document.dump(2) + "\n");
}

/** Compare store enums in assertions without adding stream operators to the API. */
template<typename Enum>
int code(Enum value) {
    return static_cast<int>(value);
}

/** Production run identities are UUIDs; archive names depend on the shape. */
std::string run_identity(unsigned index) {
    char text[37]{};
    std::snprintf(text, sizeof(text), "11111111-1111-4111-8111-%012u", index);
    return text;
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
 * A state that exercises everything an archive must reproduce.
 *
 * Large plain text, base64 binary content, tool declarations, reasoning,
 * invocation provenance, loop progress with an unprojected result batch and
 * open JSON extras: the archive is a complete state, not a rendered summary.
 */
model_io::AgentInputState rich_state() {
    model_io::AgentInputState state;
    state.meta.session_id = "session-1";
    state.meta.created_at = "2026-10-17T00:00:00Z";
    state.meta.updated_at = "2026-10-17T01:00:00Z";
    state.meta.error = Json{{"kind", "fixture"}, {"message", "retained"}};
    state.system_prompt.add_section("system", "Instructions", "Be precise.");
    state.system_prompt.add_section("memory.runtime", "Memory", "Old summary.",
        model_io::SectionStability::Volatile);
    model_io::Invocable tool;
    tool.name = "probe";
    tool.description = "Inspect a local value.";
    tool.argument_schema = {{"type", "object"},
        {"properties", {{"path", {{"type", "string"}}}}}};
    tool.remote_type = "fixture";
    tool.extras = Json{{"plugin", "retained"}};
    state.tools.push_back(tool);

    model_io::UserLoopStep turn;
    turn.user_input.type = model_io::MessageItemType::UserInput;
    turn.user_input.role = "user";
    turn.user_input.content.push_back(
        {model_io::ContentType::Text, std::string(1024 * 1024, 'L'),
         std::nullopt, model_io::Modality::Text});
    turn.user_input.extras = Json{{"custom", {{"nested", {1, 2, 3}}}}};
    model_io::AgentLoopStep step;
    step.model_response.type = model_io::MessageItemType::ModelResponse;
    step.model_response.role = "assistant";
    step.model_response.content.push_back(
        {model_io::ContentType::Binary, std::string("AAECAwQFBgcICQ=="), std::nullopt,
         model_io::Modality::Image});
    step.model_response.reasoning = model_io::Content{
        model_io::ContentType::Text, "private reasoning", std::nullopt,
        model_io::Modality::Text};
    step.model_response.cost = model_io::TokenCost{100, 20, 50};
    model_io::InvokeQuery query;
    query.id = "call-1";
    query.name = "probe";
    query.arguments = {{"path", "/tmp/value"}};
    step.model_response.invokes = std::vector<model_io::InvokeQuery>{query};
    step.commit_sequence = 7;
    step.extras = Json{{"simplex.execution", "fixture"}};
    turn.agent_loop_step.push_back(std::move(step));
    state.turns.push_back(std::move(turn));

    state.loop.emplace();
    state.loop->status = model_io::LoopStatus::Completed;
    state.loop->phase = model_io::LoopPhase::Projection;
    state.loop->completed_exchanges = 1;
    state.loop->committed_response_sequence = 7;
    model_io::InvokeReturn result;
    result.query = query;
    result.output = model_io::Content{
        model_io::ContentType::Text, std::string("binary\x00payload", 14),
        std::nullopt, model_io::Modality::Text};
    result.extras = Json{{"status", "ok"}};
    state.loop->pending_results.push_back(std::move(result));
    state.extras = Json{{"custom", "retained"},
        {"external_status", {{"context_statistic", {{"samples", 3}}}}}};
    return state;
}

} // namespace

BOOST_AUTO_TEST_CASE(archive_round_trips_the_complete_precompact_state) {
    Scratch scratch;
    auto store = persistence::make_json_store("session-1", {scratch.state(), scratch.memory()});
    const auto original = rich_state();
    const auto archive = archive_once(*store, original, run_identity(1));

    BOOST_TEST(code(archive.backend) == code(Backend::Json));
    BOOST_TEST(code(archive.kind) == code(ArchiveKind::JsonState));
    BOOST_TEST(archive.restorable());
    BOOST_TEST(archive.payload.filename().string() == "state.json");

    // The manifest is the publication marker: identity, format and payload size.
    const auto manifest = read_json(archive.payload.parent_path() / "manifest.json");
    BOOST_TEST(manifest.at("format").get<std::string>() == "simplex-compact-archive");
    BOOST_TEST(manifest.at("version").get<int>() == 1);
    BOOST_TEST(manifest.at("archive_id").get<std::string>() == archive.archive_id);
    BOOST_TEST(manifest.at("session_id").get<std::string>() == "session-1");
    BOOST_TEST(manifest.at("backend").get<std::string>() == "json");
    BOOST_TEST(manifest.at("kind").get<std::string>() == "json_state");
    BOOST_TEST(manifest.at("payload").at("file").get<std::string>() == "state.json");
    BOOST_TEST(manifest.at("payload").at("bytes").get<std::uint64_t>()
        == fs::file_size(archive.payload));

    // The payload is the complete original state, not a rendered summary.
    const auto decoded = load::load_state(archive.payload);
    BOOST_TEST(Json(decoded) == Json(original));
    BOOST_REQUIRE(decoded.loop.has_value());
    BOOST_TEST(decoded.loop->pending_results.size() == 1u);
    BOOST_TEST(decoded.turns.at(0).user_input.content.at(0).raw.size() == 1024u * 1024u);
    BOOST_TEST(code(decoded.turns.at(0).agent_loop_step.at(0).model_response.content.at(0).type)
        == code(model_io::ContentType::Binary));
    BOOST_TEST(decoded.extras.value().at("custom") == "retained");
}

BOOST_AUTO_TEST_CASE(recognition_requires_a_complete_matching_archive) {
    Scratch scratch;
    auto store = persistence::make_json_store("session-1", {scratch.state(), scratch.memory()});
    const auto original = rich_state();
    const auto archive = archive_once(*store, original, run_identity(1));
    const auto manifest_path = archive.payload.parent_path() / "manifest.json";
    const auto manifest = read_json(manifest_path);

    /** Apply one edit, prove it is not evidence, then restore the archive. */
    const auto rejects = [&](const Json& damaged, const auto& extra) {
        write_json(manifest_path, damaged);
        extra();
        BOOST_CHECK(!store->archive_at(archive.payload.parent_path()).has_value());
        BOOST_CHECK(!store->archive_at(archive.payload).has_value());
        write_json(manifest_path, manifest);
    };

    auto other_session = manifest;
    other_session["session_id"] = "another-session";
    rejects(other_session, [] {});

    auto wrong_size = manifest;
    wrong_size["payload"]["bytes"] = manifest.at("payload").at("bytes").get<std::uint64_t>() + 1;
    rejects(wrong_size, [] {});

    auto other_format = manifest;
    other_format["format"] = "other-format";
    rejects(other_format, [] {});

    // An extra file means the directory is no longer exactly one archive.
    rejects(manifest, [&] { write_file(archive.payload.parent_path() / "notes.txt", "x"); });
    BOOST_REQUIRE(fs::remove(archive.payload.parent_path() / "notes.txt"));

    // Without its manifest an otherwise complete payload is incomplete evidence.
    BOOST_REQUIRE(fs::remove(manifest_path));
    BOOST_CHECK(!store->archive_at(archive.payload).has_value());
    write_json(manifest_path, manifest);

    const auto recognized = store->archive_at(archive.payload);
    BOOST_REQUIRE(recognized.has_value());
    BOOST_TEST(recognized->archive_id == archive.archive_id);
    BOOST_TEST(code(recognized->kind) == code(ArchiveKind::JsonState));
}

BOOST_AUTO_TEST_CASE(legacy_markdown_is_evidence_but_never_restorable_or_deleted) {
    Scratch scratch;
    auto store = persistence::make_json_store("session-1", {scratch.state(), scratch.memory()});
    fs::create_directories(scratch.memory());
    const auto legacy = scratch.memory() / ("00000000000000000001-2026-10-17T000000Z-"
        + run_identity(1));
    fs::create_directory(legacy);
    write_file(legacy / "state.md", "# archived Markdown\n");

    const auto recognized = store->archive_at(legacy);
    BOOST_REQUIRE(recognized.has_value());
    BOOST_TEST(code(recognized->kind) == code(ArchiveKind::LegacyReadable));
    BOOST_TEST(!recognized->restorable());
    BOOST_TEST(recognized->payload.filename().string() == "state.md");
    // Historical states name the payload file; the same archive must resolve.
    BOOST_TEST(store->archive_at(legacy / "state.md").has_value());

    // A later retention pass leaves legacy evidence alone even above the target.
    const auto current = archive_once(*store, rich_state(), run_identity(2));
    ArchivePruneRequest prune;
    prune.current = current;
    prune.referenced = {*recognized};
    prune.policy.max_archives = 1;
    BOOST_TEST(store->prune_archives(prune).removed_archives == 0u);
    BOOST_TEST(fs::exists(legacy / "state.md"));
    BOOST_TEST(fs::exists(current.payload));
}

BOOST_AUTO_TEST_CASE(retention_keeps_the_current_and_referenced_archives) {
    Scratch scratch;
    auto store = persistence::make_json_store("session-1", {scratch.state(), scratch.memory()});
    const auto original = rich_state();
    const auto referenced = archive_once(*store, original, run_identity(1));
    const auto obsolete = archive_once(*store, original, run_identity(2));
    const auto current = archive_once(*store, original, run_identity(3));
    // Neither an unknown name nor a symlink is this store's archive.
    const auto notes = scratch.memory() / "notes.txt";
    write_file(notes, "operator notes");
    const auto unknown = scratch.memory() / "unknown-directory";
    fs::create_directories(unknown);
    write_file(unknown / "state.md", "not the recognized name pattern");
    const auto linked = scratch.memory() / ("00000000000000000009-2026-10-17T000000Z-"
        + run_identity(9));
    fs::create_directory_symlink(obsolete.payload.parent_path(), linked);

    ArchivePruneRequest prune;
    prune.current = current;
    // Retained state names the payload file of the referenced attempt.
    const auto mapped = store->archive_at(referenced.payload);
    BOOST_REQUIRE(mapped.has_value());
    prune.referenced = {*mapped};
    prune.policy.max_archives = 2;
    const auto cleaned = store->prune_archives(prune);
    BOOST_TEST(cleaned.removed_archives == 1u);
    BOOST_TEST(cleaned.removed_bytes > 0u);
    BOOST_TEST(fs::exists(referenced.payload));
    BOOST_TEST(!fs::exists(obsolete.payload.parent_path()));
    BOOST_TEST(fs::exists(current.payload));
    BOOST_TEST(fs::exists(notes));
    BOOST_TEST(fs::exists(unknown / "state.md"));
    BOOST_TEST(fs::is_symlink(linked));

    // Zero disables cleanup entirely.
    prune.policy.max_archives = 0;
    BOOST_TEST(store->prune_archives(prune).removed_archives == 0u);
    BOOST_TEST(fs::exists(current.payload));
}

BOOST_AUTO_TEST_CASE(references_resolve_only_inside_recognized_archives) {
    Scratch scratch;
    auto store = persistence::make_json_store("session-1", {scratch.state(), scratch.memory()});
    const auto archive = archive_once(*store, rich_state(), run_identity(1));

    BOOST_TEST(!store->archive_at({}).has_value());
    // A direct child may be named as the directory or as a file inside it: the
    // memory prompt records the payload path, and reference scanning may strip
    // the extension, so the directory is the identity either way.
    BOOST_TEST(store->archive_at(archive.payload.parent_path()).has_value());
    BOOST_TEST(store->archive_at(archive.payload).has_value());
    BOOST_TEST(store->archive_at(archive.payload.parent_path() / "state").has_value());
    // Nested and foreign paths are not evidence this store owns.
    BOOST_TEST(!store->archive_at(
        archive.payload.parent_path() / "nested" / "deeper").has_value());
    BOOST_TEST(!store->archive_at(scratch.root / "elsewhere").has_value());
    BOOST_TEST(!store->archive_at(scratch.memory()).has_value());
}

BOOST_AUTO_TEST_CASE(prune_rejects_a_current_archive_it_cannot_verify) {
    Scratch scratch;
    auto store = persistence::make_json_store("session-1", {scratch.state(), scratch.memory()});
    const auto current = archive_once(*store, rich_state(), run_identity(1));
    ArchivePruneRequest prune;
    prune.current = current;
    prune.policy.max_archives = 1;
    BOOST_TEST(store->prune_archives(prune).removed_archives == 0u);

    auto foreign = current;
    foreign.session_id = "another-session";
    prune.current = foreign;
    BOOST_CHECK_THROW((void)store->prune_archives(prune), std::invalid_argument);

    auto misplaced = current;
    misplaced.payload = scratch.root / "elsewhere/state.json";
    prune.current = misplaced;
    BOOST_CHECK_THROW((void)store->prune_archives(prune), std::invalid_argument);

    // A directory that is not a complete archive cannot be protected or pruned.
    fs::create_directories(scratch.memory() / ("00000000000000000002-2026-10-17T000000Z-"
        + run_identity(2)));
    auto incomplete = current;
    incomplete.archive_id = "00000000000000000002-2026-10-17T000000Z-" + run_identity(2);
    incomplete.payload = scratch.memory() / incomplete.archive_id / "state.json";
    prune.current = incomplete;
    BOOST_CHECK_THROW((void)store->prune_archives(prune), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(disabled_retention_still_requires_the_current_archive) {
    Scratch scratch;
    auto store = persistence::make_json_store("session-1", {scratch.state(), scratch.memory()});
    const auto current = archive_once(*store, rich_state(), run_identity(1));
    ArchivePruneRequest prune;
    prune.current = current;
    prune.policy.max_archives = 0;
    BOOST_TEST(store->prune_archives(prune).removed_archives == 0u);

    // Retention zero must not report success for a store whose own attempt is
    // missing: the caller would treat that as verified evidence.
    auto missing = current;
    missing.archive_id = "00000000000000000009-2026-10-17T000000Z-" + run_identity(9);
    missing.payload = scratch.memory() / missing.archive_id / "state.json";
    prune.current = missing;
    BOOST_CHECK_THROW((void)store->prune_archives(prune), persistence::StoreError);
}
