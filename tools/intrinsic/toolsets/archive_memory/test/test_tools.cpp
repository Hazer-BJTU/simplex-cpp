#define BOOST_TEST_MODULE ArchiveMemoryTools
#include <boost/test/unit_test.hpp>
#include "tools/intrinsic/archive_memory/toolset.hpp"
#include "tools/intrinsic/archive_memory/schemas.hpp"
#include "tools/registry.hpp"
#include "tools/invoke_exception.hpp"
#include "archive_fixture.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_future.hpp>
#include <atomic>
#include <fstream>
#include <thread>

namespace {
namespace asio = boost::asio;
using Json = nlohmann::json;

/** Real store, registry and independently running storage executor. */
struct Fixture : archive_test::Fixture {
    asio::io_context io;
    asio::thread_pool storage{1};
    tools::ToolRegistry registry;

    Fixture() { install(persistence::make_sqlite_archive_reader("test", root)); }
    ~Fixture() { storage.join(); }

    void install(std::shared_ptr<const persistence::ArchiveReader> reader)
    {
        registry.add(std::make_shared<tools::intrinsic::ArchiveMemoryToolSet>(
            std::move(reader), storage.get_executor()));
    }

    model_io::InvokeReturn call(const std::string& name, Json arguments)
    {
        model_io::InvokeQuery query;
        query.name = name;
        query.id = "query";
        query.arguments = std::move(arguments);
        io.restart();
        auto future = asio::co_spawn(io, registry.execute({query}, io.get_executor()), asio::use_future);
        io.run();
        auto results = future.get();
        BOOST_REQUIRE_EQUAL(results.size(), 1u);
        return std::move(results.front());
    }

    persistence::ArchiveHit hit(std::string text)
    {
        archive(state(text));
        persistence::ArchiveSearchRequest request;
        request.text = text.substr(0, 3);
        auto found = persistence::make_sqlite_archive_reader("test", root)->search(request);
        BOOST_REQUIRE_EQUAL(found.hits.size(), 1u);
        return found.hits.front();
    }
};

void contains(const model_io::InvokeReturn& result, std::string_view text)
{
    BOOST_TEST(result.output.raw.find(text) != std::string::npos,
               "Missing " << text << " in " << result.output.raw);
}

std::uint64_t field(const model_io::InvokeReturn& result, const std::string& key)
{
    const auto label = "[[" + key + "]]: ";
    const auto start = result.output.raw.find(label);
    BOOST_REQUIRE(start != std::string::npos);
    return std::stoull(result.output.raw.substr(start + label.size()));
}

/** Authoritative schema override, restored before any other test constructs tools. */
struct SchemaOverride {
    std::optional<std::string> previous;
    explicit SchemaOverride(const std::filesystem::path& directory)
    {
        if (const auto old = std::getenv("SIMPLEX_ARCHIVE_MEMORY_SCHEMA_DIR")) previous = old;
        if (::setenv("SIMPLEX_ARCHIVE_MEMORY_SCHEMA_DIR", directory.c_str(), 1) != 0) {
            throw std::runtime_error("cannot override schemas");
        }
    }
    ~SchemaOverride()
    {
        if (previous) ::setenv("SIMPLEX_ARCHIVE_MEMORY_SCHEMA_DIR", previous->c_str(), 1);
        else ::unsetenv("SIMPLEX_ARCHIVE_MEMORY_SCHEMA_DIR");
    }
};
}

BOOST_FIXTURE_TEST_CASE(search_then_fetch_uses_real_archive_and_trusted_readonly_routing, Fixture)
{
    const auto found = hit("Keep the deployment key location, not its value.");
    const auto search = call("memory_search", {{"query", "deployment"}});
    BOOST_REQUIRE(!tools::is_error(search));
    contains(search, found.archive.archive_id);
    contains(search, found.source_id);
    contains(search, "deployment key location");
    contains(search, "[[kind]]: user");
    BOOST_TEST(search.query.arguments["limit"] == 5);
    BOOST_CHECK(search.query.type == model_io::InvokeType::ReadOnly);
    BOOST_CHECK(search.query.security == model_io::InvokeSecurity::Trusted);
    auto result = call("memory_fetch", {{"archive_id", found.archive.archive_id},
        {"source_id", found.source_id}});
    BOOST_REQUIRE(!tools::is_error(result));
    contains(result, "Keep the deployment key location, not its value.");
    contains(result, "[[has_more]]: false");
    BOOST_TEST(result.query.arguments["length"] == 8192);
    model_io::PromptTemplate prompt;
    BOOST_TEST(registry.inject_skills(prompt) == 1u);
    BOOST_TEST(prompt.render().markdown.find("Searching archived memory") != std::string::npos);
    for (const auto& tool : registry.get_tools()) {
        BOOST_TEST(!tool.argument_schema["properties"].contains("timeout_ms"));
        BOOST_TEST(tool.argument_schema["additionalProperties"] == false);
    }
}

BOOST_FIXTURE_TEST_CASE(missing_store_and_unarchived_state_are_empty_searches_without_creation, Fixture)
{
    auto result = call("memory_search", {{"query", "needle"}});
    BOOST_REQUIRE(!tools::is_error(result));
    contains(result, "[[hits]]: 0");
    BOOST_TEST(!std::filesystem::exists(root));
    store->checkpoint(state("needle"), persistence::Boundary::RunFinished);
    result = call("memory_search", {{"query", "needle"}});
    contains(result, "[[hits]]: 0");
}

BOOST_FIXTURE_TEST_CASE(literal_unicode_search_and_fetch_ranges, Fixture)
{
    const auto found = hit("中文资料 alpha AND beta");
    auto result = call("memory_search", {{"query", "中文资料"}});
    BOOST_REQUIRE(!tools::is_error(result));
    contains(result, "中文资料");
    result = call("memory_search", {{"query", "alpha AND beta"}});
    contains(result, "[[hits]]: 1");
    const Json locator = {{"archive_id", found.archive.archive_id}, {"source_id", found.source_id}};
    auto arguments = locator;
    arguments["offset"] = 1;
    BOOST_TEST(tools::is_error(call("memory_fetch", arguments)));
    arguments["offset"] = 0;
    arguments["length"] = 4;
    result = call("memory_fetch", arguments);
    BOOST_REQUIRE(!tools::is_error(result));
    BOOST_TEST(field(result, "next_offset") == 3u);
    contains(result, "中");
    arguments["offset"] = std::string("中文资料 alpha AND beta").size();
    result = call("memory_fetch", arguments);
    BOOST_REQUIRE(!tools::is_error(result));
    contains(result, "[[has_more]]: false");
}

BOOST_FIXTURE_TEST_CASE(display_budget_preserves_original_byte_pagination, Fixture)
{
    const std::string raw = "abc" + std::string(30000, '\xff') + std::string(1, '\0') + "中文 END";
    const auto found = hit(raw);
    std::uint64_t offset = 0;
    std::size_t pages = 0;
    std::string displayed;
    while (offset < raw.size() && pages < 20) {
        const auto result = call("memory_fetch", {{"archive_id", found.archive.archive_id},
            {"source_id", found.source_id}, {"offset", offset}, {"length", 32768}});
        BOOST_REQUIRE(!tools::is_error(result));
        BOOST_TEST(result.output.raw.size() <= 32768u);
        BOOST_CHECK_NO_THROW(Json(result.output.raw).dump());
        const auto next = field(result, "next_offset");
        BOOST_REQUIRE(next > offset);
        BOOST_TEST(next <= raw.size());
        if (pages == 0) {
            contains(result, "[[output_truncated]]: true");
            contains(result, "[[display_replaced]]: true");
        }
        // Collect exactly the content, excluding ToolResult's trailing newline.
        const auto body = result.output.raw.find(" bytes):\n");
        BOOST_REQUIRE(body != std::string::npos);
        auto part = result.output.raw.substr(body + std::string(" bytes):\n").size());
        if (part.ends_with('\n')) part.pop_back();
        displayed += part;
        offset = next;
        ++pages;
    }
    BOOST_TEST(offset == raw.size());
    std::string expected = "abc";
    for (int i = 0; i < 30001; ++i) expected += "\xef\xbf\xbd";
    expected += "中文 END";
    BOOST_TEST(displayed == expected);
}

BOOST_FIXTURE_TEST_CASE(invalid_arguments_never_reach_storage, Fixture)
{
    for (auto args : std::vector<Json>{
             {{"query", ""}}, {{"query", " \n"}}, {{"query", std::string(257, 'x')}},
             {{"query", std::string(1, '\xff')}}, {{"query", std::string("a\0b", 3)}},
             {{"query", "needle"}, {"path", "/elsewhere"}},
             {{"query", "needle"}, {"limit", 0}}, {{"query", "needle"}, {"limit", 21}},
             {{"query", "needle"}, {"limit", 1.5}}}) {
        BOOST_TEST(tools::is_error(call("memory_search", args)));
    }
    for (auto args : std::vector<Json>{
             {{"archive_id", "a"}}, {{"archive_id", "a"}, {"source_id", "s"}, {"length", 3}},
             {{"archive_id", "a"}, {"source_id", "s"}, {"offset", -1}},
             {{"archive_id", "a"}, {"source_id", "s"}, {"length", 32769}},
             {{"archive_id", "a"}, {"source_id", "s"}, {"session_id", "other"}}}) {
        BOOST_TEST(tools::is_error(call("memory_fetch", args)));
    }
    BOOST_TEST(!std::filesystem::exists(root));
}

BOOST_FIXTURE_TEST_CASE(pruned_archive_and_wrong_session_are_errors_not_empty_results, Fixture)
{
    const auto found = hit("needle in archive");
    sqlite::Database database(path());
    database.execute("PRAGMA foreign_keys = ON; DELETE FROM archives;");
    auto result = call("memory_fetch", {{"archive_id", found.archive.archive_id},
        {"source_id", found.source_id}});
    BOOST_TEST(tools::is_error(result));
    contains(result, "not_found");
    tools::ToolRegistry other;
    other.add(std::make_shared<tools::intrinsic::ArchiveMemoryToolSet>(
        persistence::make_sqlite_archive_reader("other", root), storage.get_executor()));
    model_io::InvokeQuery query;
    query.name = "memory_search";
    query.arguments = {{"query", "needle"}};
    io.restart();
    auto future = asio::co_spawn(io, other.execute({query}, io.get_executor()), asio::use_future);
    io.run();
    result = future.get().front();
    BOOST_TEST(tools::is_error(result));
    contains(result, "storage_error");
}

BOOST_FIXTURE_TEST_CASE(old_index_is_not_migrated_by_a_query, Fixture)
{
    hit("needle");
    sqlite::Database database(path());
    archive_test::downgrade_to_v1(database);
    const auto result = call("memory_search", {{"query", "needle"}});
    BOOST_TEST(tools::is_error(result));
    contains(result, "index_unavailable");
    BOOST_TEST(archive_test::scalar(database, "SELECT store_schema_version FROM store_meta") == 1);
}

BOOST_FIXTURE_TEST_CASE(host_configuration_bounds_whole_results_and_disables_invalid_tool, Fixture)
{
    for (int i = 0; i < 20; ++i) archive(state("needle " + std::to_string(i) + std::string(700, 'x')));
    const auto schemas = root / "schemas";
    std::filesystem::copy(tools::intrinsic::archive_memory::schema_directory(), schemas,
        std::filesystem::copy_options::recursive);
    const auto file = schemas / "memory_search.yaml";
    std::ifstream input(file);
    std::string yaml{std::istreambuf_iterator<char>(input), {}};
    yaml.replace(yaml.find("max_output_bytes: 32768"), std::string("max_output_bytes: 32768").size(), "max_output_bytes: 2048");
    std::ofstream(file) << yaml;
    SchemaOverride override(schemas);
    tools::ToolRegistry set;
    set.add(std::make_shared<tools::intrinsic::ArchiveMemoryToolSet>(
        persistence::make_sqlite_archive_reader("test", root), storage.get_executor()));
    model_io::InvokeQuery query;
    query.name = "memory_search";
    query.arguments = {{"query", "needle"}, {"limit", 20}};
    io.restart();
    auto future = asio::co_spawn(io, set.execute({query}, io.get_executor()), asio::use_future);
    io.run();
    const auto result = future.get().front();
    BOOST_REQUIRE(!tools::is_error(result));
    BOOST_TEST(result.output.raw.size() <= 2048u);
    BOOST_TEST(field(result, "hits") >= 1u);
    BOOST_TEST(field(result, "hits") < 20u);
    contains(result, "[[has_more]]: true");
    yaml.replace(yaml.find("timeout_ms: 1000"), std::string("timeout_ms: 1000").size(), "timeout_ms: 0");
    std::ofstream(file) << yaml;
    tools::intrinsic::ArchiveMemoryToolSet invalid(
        persistence::make_sqlite_archive_reader("test", root), storage.get_executor());
    BOOST_TEST(invalid.get_tools().size() == 1u);
    BOOST_TEST(invalid.get_tools().front().name == "memory_fetch");
}

/** Assert executor placement and error mapping without a slow or flaky SQL query. */
struct ProbeReader : persistence::ArchiveReader {
    std::thread::id caller = std::this_thread::get_id();
    mutable std::atomic<bool> off_caller{false};
    persistence::ArchivePage list(std::size_t, std::optional<std::int64_t>,
                                  const persistence::ArchiveQueryOptions&) const override { return {}; }
    persistence::ArchiveSearchResult search(const persistence::ArchiveSearchRequest&,
                                            const persistence::ArchiveQueryOptions&) const override
    {
        off_caller = std::this_thread::get_id() != caller;
        throw persistence::ArchiveQueryError(persistence::ArchiveQueryFailure::TimedOut, "private diagnostic");
    }
    persistence::ArchiveExcerpt read(const std::string&, const std::string&, std::uint64_t,
                                    std::size_t, const persistence::ArchiveQueryOptions&) const override { return {}; }
};

BOOST_AUTO_TEST_CASE(query_runs_off_network_executor_and_reports_timeout_without_private_details)
{
    asio::io_context io;
    asio::thread_pool storage(1);
    auto reader = std::make_shared<ProbeReader>();
    tools::ToolRegistry set;
    set.add(std::make_shared<tools::intrinsic::ArchiveMemoryToolSet>(reader, storage.get_executor()));
    model_io::InvokeQuery query;
    query.name = "memory_search";
    query.arguments = {{"query", "needle"}};
    auto future = asio::co_spawn(io, set.execute({query}, io.get_executor()), asio::use_future);
    io.run();
    const auto result = future.get().front();
    BOOST_TEST(reader->off_caller.load());
    BOOST_TEST(tools::is_error(result));
    contains(result, "timed_out");
    BOOST_TEST(result.output.raw.find("private diagnostic") == std::string::npos);
    storage.join();
}
