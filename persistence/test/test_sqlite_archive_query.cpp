#define BOOST_TEST_MODULE SqliteArchiveQuery
#include <boost/test/unit_test.hpp>

#include "archive_fixture.hpp"
#include "persistence/archive_query.hpp"
#include "sqlite_schema.hpp"

#include <future>
#include <thread>

using namespace archive_test;
using namespace persistence;

namespace {
template<class Function>
void expect_failure(Function function, ArchiveQueryFailure reason) {
    try {
        function();
        BOOST_FAIL("expected archive query error");
    } catch (const ArchiveQueryError& error) {
        BOOST_TEST(static_cast<int>(error.reason()) == static_cast<int>(reason));
    }
}
}

BOOST_FIXTURE_TEST_CASE(list_search_and_read_preserve_location_and_outcome, Fixture) {
    const auto old = archive(state("old evidence"));
    auto value = state("the SQLite configuration lives in /workspace/config.yaml; 数据库配置路径");
    const auto latest = archive(value);
    const auto reader = make_sqlite_archive_reader("test", root);
    auto page = reader->list(1);
    BOOST_REQUIRE_EQUAL(page.archives.size(), 1u);
    BOOST_TEST(page.has_more);
    BOOST_TEST(page.archives[0].archive_id == latest.archive_id);
    BOOST_CHECK(page.archives[0].outcome == ArchivedAttemptStatus::Failed);
    page = reader->list(1, page.archives.back().sequence);
    BOOST_REQUIRE_EQUAL(page.archives.size(), 1u);
    BOOST_TEST(page.archives[0].archive_id == old.archive_id);
    BOOST_TEST(!page.has_more);

    for (const auto* text : {"SQLITE", "数据库配置", "/workspace/config.yaml"}) {
        auto found = reader->search({.text = text});
        BOOST_REQUIRE_EQUAL(found.hits.size(), 1u);
        const auto& hit = found.hits[0];
        BOOST_TEST(hit.archive.archive_id == latest.archive_id);
        BOOST_CHECK(hit.turn == 0);
        BOOST_TEST(!hit.step.has_value());
        BOOST_CHECK(hit.kind == ArchiveTextKind::User);
        const auto full = reader->read(hit.archive.archive_id, hit.source_id);
        BOOST_TEST(full.text == value.turns[0].user_input.content[0].raw);
        BOOST_TEST(!full.has_more);
    }
}

BOOST_FIXTURE_TEST_CASE(literal_queries_short_queries_and_filters, Fixture) {
    const auto ref = archive(state("literal \"quote\" OR NOT * % _ 和中文；只读；X"));
    archive(state("unrelated evidence"));
    auto reader = make_sqlite_archive_reader("test", root);
    for (const auto* text : {"\"quote\" OR NOT *", "%", "只读", "X", "中文；"}) {
        auto result = reader->search({.text = text});
        BOOST_REQUIRE_EQUAL(result.hits.size(), 1u);
        BOOST_TEST(result.hits[0].archive.archive_id == ref.archive_id);
    }
    BOOST_TEST(reader->search({.text = "missing OR literal"}).hits.empty());
    BOOST_TEST(reader->search({.text = "literal", .kind = ArchiveTextKind::Assistant}).hits.empty());
    BOOST_TEST(reader->search({.text = "literal", .archive_id = "absent"}).hits.empty());
    expect_failure([&] { reader->search({.text = ""}); }, ArchiveQueryFailure::InvalidArgument);
    expect_failure([&] { reader->search({.text = std::string(257, 'x')}); }, ArchiveQueryFailure::InvalidArgument);
    expect_failure([&] { reader->search({.text = std::string(1, char(0xff))}); }, ArchiveQueryFailure::InvalidArgument);
    expect_failure([&] { reader->search({.text = std::string("a\0b", 3)}); }, ArchiveQueryFailure::InvalidArgument);
    expect_failure([&] { reader->list(0); }, ArchiveQueryFailure::InvalidArgument);
    expect_failure([&] { reader->list(1, -1); }, ArchiveQueryFailure::InvalidArgument);
}

BOOST_FIXTURE_TEST_CASE(overlap_and_duplicate_archives_do_not_fill_result_page, Fixture) {
    const auto text = std::string(3980, 'a') + " crossingneedle " + std::string(5000, 'b');
    archive(state(text));
    const auto latest = archive(state(text));
    auto reader = make_sqlite_archive_reader("test", root);
    auto result = reader->search({.text = "crossingneedle", .limit = 1, .snippet_bytes = 32});
    BOOST_REQUIRE_EQUAL(result.hits.size(), 1u);
    BOOST_TEST(result.hits[0].archive.archive_id == latest.archive_id);
    BOOST_TEST(result.hits[0].snippet.size() <= 32u);
    BOOST_TEST(result.hits[0].truncated);
    // Different overlapping chunks in repeated archives may still yield a second
    // distinct projection; exact chunk duplicates are suppressed independently.
    const auto ref = archive(state("separate crossingneedle evidence"));
    result = reader->search({.text = "crossingneedle", .limit = 1});
    BOOST_TEST(result.has_more);
    BOOST_REQUIRE_EQUAL(result.hits.size(), 1u);
    BOOST_TEST(!ref.archive_id.empty());
}

BOOST_FIXTURE_TEST_CASE(cross_chunk_phrase_and_tail_remain_searchable, Fixture) {
    const auto value = state(std::string(4080, 'x') + " querycrossingthechunkboundary "
        + std::string(10000, 'y') + " uniquetail");
    archive(value);
    auto reader = make_sqlite_archive_reader("test", root);
    for (const auto* text : {"querycrossingthechunkboundary", "uniquetail"}) {
        auto result = reader->search({.text = text});
        BOOST_REQUIRE(!result.hits.empty());
        const auto& hit = result.hits[0];
        const auto excerpt = reader->read(hit.archive.archive_id, hit.source_id, hit.start_byte, 4096);
        BOOST_TEST(excerpt.text.find(text) != std::string::npos);
    }
}

BOOST_FIXTURE_TEST_CASE(read_pages_preserve_utf8_nul_and_invalid_bytes, Fixture) {
    std::string text = "a🙂中文";
    text += std::string("\0middle", 7);
    text += std::string(10, char(0x80));
    text += "z";
    const auto ref = archive(state(text));
    auto reader = make_sqlite_archive_reader("test", root);
    const auto hits = reader->search({.text = "middle"});
    BOOST_REQUIRE_EQUAL(hits.hits.size(), 1u);
    const auto& id = hits.hits[0].source_id;
    std::string rebuilt;
    std::uint64_t offset = 0;
    do {
        const auto slice = reader->read(ref.archive_id, id, offset, 4);
        BOOST_TEST(slice.total_bytes == text.size());
        BOOST_TEST(slice.end_byte > offset);
        rebuilt += slice.text;
        offset = slice.end_byte;
        if (!slice.has_more) break;
    } while (offset < text.size());
    BOOST_TEST(rebuilt == text);
    BOOST_TEST(reader->read(ref.archive_id, id, offset).text.empty());
    expect_failure([&] { reader->read(ref.archive_id, id, 2); }, ArchiveQueryFailure::InvalidArgument);
    expect_failure([&] { reader->read(ref.archive_id, id, text.size() + 1); }, ArchiveQueryFailure::InvalidArgument);
}

BOOST_FIXTURE_TEST_CASE(reads_never_create_upgrade_or_cross_session_boundaries, Fixture) {
    auto reader = make_sqlite_archive_reader("test", root);
    expect_failure([&] { reader->list(); }, ArchiveQueryFailure::MissingStore);
    BOOST_TEST(!std::filesystem::exists(root));
    archive(state("query-private-marker"));
    auto wrong = make_sqlite_archive_reader("other", root);
    expect_failure([&] { wrong->search({.text = "query-private-marker"}); }, ArchiveQueryFailure::StorageError);
    {
        sqlite::Database db(path());
        downgrade_to_v1(db);
    }
    expect_failure([&] { reader->list(); }, ArchiveQueryFailure::IndexUnavailable);
    sqlite::Database db(path(), sqlite::OpenMode::ReadOnly);
    BOOST_TEST(scalar(db, "SELECT store_schema_version FROM store_meta") == 1);
    BOOST_TEST(scalar(db, "SELECT count(*) FROM sqlite_master WHERE name = 'archive_chunks'") == 0);
}

BOOST_FIXTURE_TEST_CASE(pruned_reference_cannot_alias_new_archive_rows, Fixture) {
    const auto old = archive(state("oldneedle"));
    auto reader = make_sqlite_archive_reader("test", root);
    const auto old_hit = reader->search({.text = "oldneedle"}).hits.at(0);
    const auto latest = archive(state("freshneedle"));
    ArchivePruneRequest request;
    request.current = latest;
    request.policy.max_archives = 1;
    store->prune_archives(request);
    archive(state("newneedle"));
    expect_failure([&] { reader->read(old.archive_id, old_hit.source_id); }, ArchiveQueryFailure::NotFound);
    BOOST_TEST(reader->search({.text = "oldneedle"}).hits.empty());
    BOOST_TEST(!reader->search({.text = "freshneedle"}).hits.empty());
}

BOOST_FIXTURE_TEST_CASE(cancellation_and_parallel_reads_do_not_poison_writes, Fixture) {
    archive(state("searchable evidence"));
    auto reader = make_sqlite_archive_reader("test", root);
    std::stop_source stop;
    stop.request_stop();
    ArchiveQueryOptions options;
    options.stop = stop.get_token();
    expect_failure([&] { reader->search({.text = "searchable"}, options); }, ArchiveQueryFailure::Cancelled);
    options.stop = {};
    options.timeout = std::chrono::milliseconds(0);
    expect_failure([&] { reader->list(10, {}, options); }, ArchiveQueryFailure::InvalidArgument);
    auto parallel = std::async(std::launch::async, [&] {
        for (int i = 0; i < 10; ++i) {
            if (reader->search({.text = "searchable"}).hits.size() != 1) return false;
        }
        return true;
    });
    for (int i = 0; i < 5; ++i) store->checkpoint(state("current"), Boundary::RunFinished);
    BOOST_TEST(parallel.get());
    BOOST_TEST(reader->search({.text = "searchable"}).hits.size() == 1u);
}

BOOST_FIXTURE_TEST_CASE(deadline_and_live_cancellation_are_explicit_and_recoverable, Fixture) {
    archive(state("normalneedle"));
    // Replace only the source view with deliberately expensive SQL. This tests
    // the public reader's deadline/cancellation translation without a huge fixture.
    std::string original_view;
    {
        sqlite::Database db(path());
        auto sql = db.prepare("SELECT sql FROM sqlite_master WHERE name = 'archive_text_sources'");
        BOOST_REQUIRE(sql.step() == sqlite::StepResult::Row);
        original_view = sql.column_text(0);
    }
    {
        sqlite::Database db(path());
        db.execute("DROP VIEW archive_text_sources; CREATE VIEW archive_text_sources AS "
            "WITH RECURSIVE work(n) AS (VALUES(0) UNION ALL SELECT n+1 FROM work WHERE n<1000000000) "
            "SELECT 'archive' AS archive_id, 'source' AS source_id, CAST(sum(n) AS TEXT) AS body FROM work;");
    }
    auto reader = make_sqlite_archive_reader("test", root);
    ArchiveQueryOptions options;
    options.timeout = std::chrono::milliseconds(1);
    expect_failure([&] { reader->read("archive", "source", 0, 32, options); }, ArchiveQueryFailure::TimedOut);
    std::stop_source stop;
    options.timeout = std::chrono::seconds(10);
    options.stop = stop.get_token();
    {
        std::jthread cancel([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            stop.request_stop();
        });
        expect_failure([&] { reader->read("archive", "source", 0, 32, options); }, ArchiveQueryFailure::Cancelled);
    }
    {
        sqlite::Database db(path());
        db.execute("DROP VIEW archive_text_sources");
        db.execute(original_view);
    }
    store->checkpoint(state("next"), Boundary::RunFinished);
    BOOST_TEST(reader->search({.text = "normalneedle"}).hits.size() == 1u);
}

BOOST_FIXTURE_TEST_CASE(maximum_utf8_query_crosses_chunk_boundary_without_losing_overlap, Fixture) {
    // A maximum-length multibyte query straddles the first chunk boundary.
    std::string needle;
    for (int i = 0; i < 85; ++i) needle += "中";
    needle += "z";
    archive(state(std::string(4095 - 253, 'a') + needle + "suffix"));
    auto reader = make_sqlite_archive_reader("test", root);
    const auto result = reader->search({.text = needle});
    BOOST_TEST(!result.hits.empty());
}

BOOST_FIXTURE_TEST_CASE(corrupt_or_foreign_store_errors_do_not_expose_search_text, Fixture) {
    archive(state("private-archive-body"));
    {
        sqlite::Database db(path());
        db.execute("DROP TRIGGER archive_chunks_insert");
    }
    auto reader = make_sqlite_archive_reader("test", root);
    try {
        reader->search({.text = "distinctive-private-query"});
        BOOST_FAIL("expected corrupt index error");
    } catch (const ArchiveQueryError& error) {
        BOOST_CHECK(error.reason() == ArchiveQueryFailure::StorageError);
        BOOST_TEST(std::string(error.what()).find("distinctive-private-query") == std::string::npos);
        BOOST_TEST(std::string(error.what()).find("private-archive-body") == std::string::npos);
    }
}

BOOST_FIXTURE_TEST_CASE(small_previews_keep_the_matching_text_visible, Fixture) {
    archive(state(std::string(100, 'a') + "只读" + std::string(100, 'b')));
    archive(state(std::string(100, 'a') + "检索位置" + std::string(100, 'b')));
    auto reader = make_sqlite_archive_reader("test", root);
    for (const auto* query : {"只读", "检索位置"}) {
        const auto result = reader->search({.text = query, .snippet_bytes = 32});
        BOOST_REQUIRE_EQUAL(result.hits.size(), 1u);
        BOOST_TEST(result.hits[0].snippet.size() <= 32u);
        BOOST_TEST(result.hits[0].snippet.find(query) != std::string::npos);
    }
}

BOOST_FIXTURE_TEST_CASE(case_folded_source_span_survives_chunk_boundaries, Fixture) {
    std::string kelvin;
    for (int i = 0; i < 256; ++i) kelvin += "K";
    const std::string query(256, 'k');
    auto reader = make_sqlite_archive_reader("test", root);
    for (const auto prefix_bytes : {100u, 3700u}) {
        const auto text = std::string(prefix_bytes, 'x') + kelvin + "suffix";
        const auto ref = archive(state(text));
        // Compare the public chunked reader with an unchunked trigram control.
        sqlite::Database control(":memory:");
        control.execute("CREATE VIRTUAL TABLE full_text USING fts5(body, tokenize='trigram')");
        auto insert = control.prepare("INSERT INTO full_text(body) VALUES(?1)");
        insert.bind_text(1, text);
        BOOST_REQUIRE(insert.step() == sqlite::StepResult::Done);
        auto match = control.prepare("SELECT count(*) FROM full_text WHERE full_text MATCH ?1");
        match.bind_text(1, query);
        BOOST_REQUIRE(match.step() == sqlite::StepResult::Row);
        BOOST_TEST(match.column_int64(0) == 1);
        const auto result = reader->search({.text = query, .archive_id = ref.archive_id,
                                            .snippet_bytes = 1024});
        BOOST_REQUIRE_EQUAL(result.hits.size(), 1u);
        BOOST_TEST(result.hits[0].snippet.find(kelvin) != std::string::npos);
        BOOST_TEST(result.hits[0].snippet.size() <= 1024u);
    }
}

BOOST_FIXTURE_TEST_CASE(previews_preserve_complete_ascii_and_unicode_matches_when_they_fit, Fixture) {
    struct Example {
        std::string query;
        std::string source_match;
        std::size_t preview_bytes;
    };
    std::string chinese;
    std::string kelvin;
    for (int i = 0; i < 80; ++i) chinese += "中";
    for (int i = 0; i < 90; ++i) kelvin += "K";
    const std::vector<Example> cases = {
        {"qqqqqqqz", "qqqqqqqz", 32},
        {std::string(89, 'q') + "z", std::string(89, 'q') + "z", 512},
        {chinese, chinese, 256},
        {std::string(90, 'k'), kelvin, 512},
        {"kkkkkkkz", "KKKKKKKz", 32},
        {"kkkkkkkkkkz", "KKKKKKKKKKz", 32},
        // A match can consume the entire allowance; no ellipsis may displace it.
        {std::string(31, 'q') + "z", std::string(31, 'q') + "z", 32},
    };
    auto reader = make_sqlite_archive_reader("test", root);
    for (const auto& example : cases) {
        BOOST_TEST_CONTEXT("query bytes=" << example.query.size() << ", budget=" << example.preview_bytes) {
            const auto ref = archive(state(std::string(100, 'x') + example.source_match + std::string(100, 'y')));
            const auto result = reader->search({.text = example.query, .archive_id = ref.archive_id,
                                               .snippet_bytes = example.preview_bytes});
            BOOST_REQUIRE_EQUAL(result.hits.size(), 1u);
            const auto& hit = result.hits[0];
            BOOST_TEST(hit.snippet.find(example.source_match) != std::string::npos);
            BOOST_TEST(hit.snippet.size() <= example.preview_bytes);
            BOOST_TEST(hit.truncated == (example.source_match.size() + 200 > example.preview_bytes));
            BOOST_CHECK_NO_THROW(nlohmann::json(hit.snippet).dump());
        }
    }
}

BOOST_FIXTURE_TEST_CASE(oversized_and_merged_matches_are_clipped_on_utf8_boundaries, Fixture) {
    std::string kelvin;
    for (int i = 0; i < 90; ++i) kelvin += "K";
    auto reader = make_sqlite_archive_reader("test", root);
    const auto large = archive(state(std::string(100, 'x') + kelvin + "suffix"));
    auto result = reader->search({.text = std::string(90, 'k'), .archive_id = large.archive_id,
                                 .snippet_bytes = 32});
    BOOST_REQUIRE_EQUAL(result.hits.size(), 1u);
    BOOST_TEST(result.hits[0].snippet == kelvin.substr(0, 30));
    BOOST_TEST(result.hits[0].truncated);
    BOOST_CHECK_NO_THROW(nlohmann::json(result.hits[0].snippet).dump());

    std::string repeated;
    for (int i = 0; i < 100; ++i) repeated += "abcd";
    const auto merged = archive(state(repeated));
    result = reader->search({.text = "abcdabcd", .archive_id = merged.archive_id, .snippet_bytes = 32});
    BOOST_REQUIRE_EQUAL(result.hits.size(), 1u);
    BOOST_TEST(result.hits[0].snippet.find("abcdabcd") != std::string::npos);
    BOOST_TEST(result.hits[0].snippet.size() <= 32u);
    BOOST_TEST(result.hits[0].truncated);
}

BOOST_FIXTURE_TEST_CASE(highlight_markers_cannot_be_spoofed_by_archived_text, Fixture) {
    const std::string text = "<simplex-match-0-open> fake <simplex-match-0-close> "
        "<simplex-match-1-close> markerneedle <simplex-match-2-open>";
    archive(state(text));
    auto reader = make_sqlite_archive_reader("test", root);
    for (const auto* query : {"markerneedle", "<simplex-match-0-open>", "<simplex-match-1-close>"}) {
        const auto result = reader->search({.text = query, .snippet_bytes = 4096});
        BOOST_REQUIRE_EQUAL(result.hits.size(), 1u);
        BOOST_TEST(result.hits[0].snippet == text);
        BOOST_TEST(!result.hits[0].truncated);
    }
    const auto small = reader->search({.text = "markerneedle", .snippet_bytes = 32});
    BOOST_REQUIRE_EQUAL(small.hits.size(), 1u);
    BOOST_TEST(small.hits[0].snippet.find("markerneedle") != std::string::npos);
    BOOST_TEST(small.hits[0].snippet.size() <= 32u);
}
