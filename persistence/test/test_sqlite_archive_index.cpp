#define BOOST_TEST_MODULE SqliteArchiveIndex
#include <boost/test/unit_test.hpp>

#include "archive_fixture.hpp"
#include "sqlite_archive_index.hpp"
#include "sqlite_schema.hpp"
#include "persistence/selection.hpp"

using namespace archive_test;

BOOST_FIXTURE_TEST_CASE(chunks_cover_full_text_and_preserve_utf8_offsets, Fixture) {
    std::string text;
    for (int i = 0; i < 3000; ++i) text += "中文🙂";
    text += " unique-tail";
    const auto ref = archive(state(text));
    sqlite::Database db(path());
    auto chunks = db.prepare("SELECT start_byte, end_byte, body FROM archive_chunks ORDER BY start_byte");
    std::size_t covered = 0;
    std::size_t count = 0;
    while (chunks.step() == sqlite::StepResult::Row) {
        const auto start = static_cast<std::size_t>(chunks.column_int64(0));
        const auto end = static_cast<std::size_t>(chunks.column_int64(1));
        BOOST_TEST(start <= covered);
        BOOST_TEST(end > covered);
        BOOST_TEST(end - start <= 4096u);
        BOOST_TEST((static_cast<unsigned char>(text[start]) & 0xc0) != 0x80);
        if (end < text.size()) BOOST_TEST((static_cast<unsigned char>(text[end]) & 0xc0) != 0x80);
        BOOST_TEST(chunks.column_text(2) == text.substr(start, end - start));
        covered = end;
        ++count;
    }
    BOOST_TEST(covered == text.size());
    BOOST_TEST(count > 1u);
    BOOST_TEST(scalar(db, "SELECT count(*) FROM archive_chunks_fts WHERE archive_chunks_fts MATCH '\"unique-tail\"'") == 1);
    BOOST_TEST(scalar(db, "SELECT count(*) FROM archive_chunks_fts WHERE archive_chunks_fts MATCH '\"中文🙂\"'") > 0);
    db.execute("INSERT INTO archive_chunks_fts(archive_chunks_fts, rank) VALUES('integrity-check', 1)");
}

BOOST_FIXTURE_TEST_CASE(projection_excludes_reasoning_binary_and_prompt_but_keeps_tool_evidence, Fixture) {
    auto value = state("userneedle");
    value.system_prompt.add_section("system", "System", "promptsecret");
    value.extras = nlohmann::json{{"private", "extrassecret"}};
    model_io::AgentLoopStep step;
    step.model_response.type = model_io::MessageItemType::ModelResponse;
    step.model_response.role = "assistant";
    model_io::Content body;
    body.raw = "assistantneedle";
    step.model_response.content.push_back(body);
    body.raw = "reasoningsecret";
    step.model_response.reasoning = body;
    body.type = model_io::ContentType::Binary;
    body.raw = "binarysecret";
    step.model_response.content.push_back(body);
    model_io::InvokeReturn result;
    result.query.name = "toolneedle";
    result.query.arguments = {{"argument", "argumentneedle"}};
    result.output.raw = "resultneedle";
    model_io::MessageItem tool;
    tool.type = model_io::MessageItemType::InvokeReturn;
    tool.invoke_return = result;
    step.invoke_returns = std::vector<model_io::MessageItem>{tool};
    value.turns[0].agent_loop_step.push_back(step);
    value.loop.emplace();
    result.output.raw = "pendingneedle";
    value.loop->pending_results.push_back(result);
    archive(value);
    sqlite::Database db(path());
    for (const auto* needle : {"userneedle", "assistantneedle", "toolneedle", "argumentneedle", "resultneedle", "pendingneedle"}) {
        auto query = db.prepare("SELECT count(*) FROM archive_chunks_fts WHERE archive_chunks_fts MATCH ?1");
        query.bind_text(1, needle);
        BOOST_REQUIRE(query.step() == sqlite::StepResult::Row);
        BOOST_TEST(query.column_int64(0) > 0);
    }
    BOOST_TEST(scalar(db, "SELECT count(*) FROM archive_chunks_fts WHERE archive_chunks_fts MATCH 'promptsecret OR reasoningsecret OR binarysecret OR extrassecret'") == 0);
}

BOOST_FIXTURE_TEST_CASE(retention_removes_index_and_current_checkpoints_do_not_enter_it, Fixture) {
    const auto old = archive(state("oldneedle"));
    const auto recent = archive(state("recentneedle"));
    store->checkpoint(state("currentsecret"), persistence::Boundary::RunFinished);
    persistence::ArchivePruneRequest request;
    request.current = recent;
    request.policy.max_archives = 1;
    BOOST_TEST(store->prune_archives(request).removed_archives == 1u);
    sqlite::Database db(path());
    BOOST_TEST(scalar(db, "SELECT count(*) FROM archive_chunks_fts WHERE archive_chunks_fts MATCH 'oldneedle OR currentsecret'") == 0);
    BOOST_TEST(scalar(db, "SELECT count(*) FROM archive_chunks_fts WHERE archive_chunks_fts MATCH 'recentneedle'") == 1);
    db.execute("INSERT INTO archive_chunks_fts(archive_chunks_fts, rank) VALUES('integrity-check', 1)");
}

BOOST_FIXTURE_TEST_CASE(index_failure_rolls_back_archive_and_original_checkpoint, Fixture) {
    store->checkpoint(state("original"), persistence::Boundary::RunFinished);
    {
        sqlite::Database db(path());
        db.execute("CREATE TRIGGER reject_chunk BEFORE INSERT ON archive_chunks BEGIN SELECT RAISE(ABORT, 'injected'); END;");
    }
    const auto ref = store->reserve_archive({"run"});
    BOOST_CHECK_THROW(store->begin_archive(ref, state("replacement")), persistence::StoreError);
    BOOST_TEST(store->load_current()->state.turns[0].user_input.content[0].raw == "original");
    sqlite::Database db(path());
    BOOST_TEST(scalar(db, "SELECT count(*) FROM archives") == 0);
    BOOST_TEST(scalar(db, "SELECT count(*) FROM snapshots WHERE kind = 'archive'") == 0);
    BOOST_TEST(scalar(db, "SELECT count(*) FROM archive_chunks") == 0);
}

BOOST_FIXTURE_TEST_CASE(v1_reads_remain_read_only_and_next_write_backfills, Fixture) {
    const auto ref = archive(state("historicalneedle"));
    const auto previous = nlohmann::json(store->load_current()->state);
    {
        sqlite::Database db(path());
        downgrade_to_v1(db);
    }
    BOOST_TEST(nlohmann::json(persistence::inspect_sqlite_current(root)->state).dump() == previous.dump());
    {
        sqlite::Database db(path(), sqlite::OpenMode::ReadOnly);
        BOOST_TEST(scalar(db, "SELECT store_schema_version FROM store_meta") == 1);
    }
    store->checkpoint(state("newcurrent"), persistence::Boundary::RunFinished);
    sqlite::Database db(path());
    BOOST_TEST(scalar(db, "SELECT store_schema_version FROM store_meta") == 2);
    BOOST_TEST(scalar(db, "SELECT count(*) FROM schema_migrations") == 2);
    BOOST_TEST(scalar(db, "SELECT count(*) FROM archive_chunks_fts WHERE archive_chunks_fts MATCH 'historicalneedle'") == 1);
    BOOST_TEST(scalar(db, "SELECT count(*) FROM archive_chunks_fts WHERE archive_chunks_fts MATCH 'newcurrent'") == 0);
}

BOOST_FIXTURE_TEST_CASE(failed_migration_preserves_v1_and_can_be_retried, Fixture) {
    archive(state("recoverableneedle"));
    {
        sqlite::Database db(path());
        downgrade_to_v1(db);
        db.execute("CREATE TABLE archive_chunks(blocker TEXT)");
    }
    BOOST_CHECK_THROW(store->checkpoint(state("next"), persistence::Boundary::RunFinished), persistence::StoreError);
    {
        sqlite::Database db(path());
        BOOST_TEST(scalar(db, "SELECT store_schema_version FROM store_meta") == 1);
        BOOST_TEST(scalar(db, "SELECT count(*) FROM schema_migrations") == 1);
        BOOST_TEST(scalar(db, "SELECT count(*) FROM sqlite_master WHERE name = 'archive_text_sources'") == 0);
        db.execute("DROP TABLE archive_chunks");
    }
    BOOST_CHECK_NO_THROW(store->checkpoint(state("retry"), persistence::Boundary::RunFinished));
    sqlite::Database db(path());
    BOOST_TEST(scalar(db, "SELECT count(*) FROM archive_chunks_fts WHERE archive_chunks_fts MATCH 'recoverableneedle'") == 1);
}

BOOST_FIXTURE_TEST_CASE(nul_and_invalid_utf8_are_only_sanitized_in_index, Fixture) {
    const std::string raw = std::string("before\0after", 12) + char(0xff) + "tailneedle";
    archive(state(raw));
    sqlite::Database db(path());
    auto source = db.prepare("SELECT body FROM archive_text_sources");
    BOOST_REQUIRE(source.step() == sqlite::StepResult::Row);
    BOOST_TEST(source.column_text(0) == raw);
    BOOST_TEST(scalar(db, "SELECT count(*) FROM archive_chunks_fts WHERE archive_chunks_fts MATCH 'tailneedle'") == 1);
}
