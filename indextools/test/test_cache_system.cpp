#define BOOST_TEST_MODULE CacheSystemTests
#include <boost/test/unit_test.hpp>

#include "cache_system.hpp"
#include "plugin_manager.hpp"

#include <boost/asio.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace indextools;
namespace fs = std::filesystem;

// The plugin output directory, injected by CMake (see test/CMakeLists.txt).
#ifndef LANG_PLUGIN_DIR
#error "LANG_PLUGIN_DIR must be defined by the build system"
#endif

namespace {

// Ensure the process-wide plugin manager is loaded from the real built plugins.
// ensure_loaded() is idempotent, so every test can call this safely.
void ensure_plugins() {
    size_t n = LangPluginManager::instance().ensure_loaded(LANG_PLUGIN_DIR);
    BOOST_REQUIRE_MESSAGE(n >= 1,
        "expected >=1 plugin in " LANG_PLUGIN_DIR ", loaded " << n);
}

// A temporary directory tree of Python sources, cleaned up on destruction.
//
//   root/
//   ├── alpha.py           (def shared_fn / class Alpha)
//   ├── beta.py            (def shared_fn / class Beta)
//   ├── notes.dat          (non-source; no plugin claims the .dat extension)
//   └── pkg/
//       └── gamma.py       (def gamma_only)
struct TreeFixture {
    fs::path root;

    TreeFixture() {
        root = fs::temp_directory_path() /
               ("indextools_cache_test_" +
                std::to_string(std::hash<std::string>{}("cache") ^
                    std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root / "pkg");

        write("alpha.py",
              "def shared_fn(x):\n"
              "    return x + 1\n"
              "\n"
              "class Alpha:\n"
              "    def method(self):\n"
              "        return shared_fn(0)\n");

        write("beta.py",
              "def shared_fn(y):\n"
              "    return y - 1\n"
              "\n"
              "class Beta:\n"
              "    pass\n");

        write("pkg/gamma.py",
              "def gamma_only():\n"
              "    return 42\n");

        write("notes.dat", "just some prose, not source code\n");
    }

    ~TreeFixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void write(const std::string& rel, const std::string& content) {
        fs::path full = root / rel;
        fs::create_directories(full.parent_path());
        std::ofstream(full) << content;
    }
};

// Drive a query to completion on a fresh io_context and return its result.
//
// The CacheSystem must run on the SAME executor we pump here, otherwise the
// spawned search coroutines would never be driven. `make_query` receives that
// executor, builds the SearchInterface, and returns the awaitable to run.
template <typename MakeQuery>
nlohmann::json run_query(MakeQuery&& make_query) {
    boost::asio::io_context ctx;
    nlohmann::json result;
    std::exception_ptr eptr;

    boost::asio::co_spawn(ctx, make_query(ctx.get_executor()),
        [&](std::exception_ptr e, nlohmann::json r) {
            eptr = e;
            result = std::move(r);
        });

    ctx.run();
    if (eptr) std::rethrow_exception(eptr);
    return result;
}

// Drive a query to completion on a multi-threaded io_context.
//
// This is the path that exercises the concurrent_channel in launch_search:
// with several worker threads calling run(), the per-slice coroutines may be
// resumed on different threads and reach their async_send concurrently. A
// single-threaded run() can never produce that interleaving, so any test that
// means to cover the channel's multi-producer safety must pump through here.
template <typename MakeQuery>
nlohmann::json run_query_threaded(unsigned threads, MakeQuery&& make_query) {
    boost::asio::io_context ctx;
    nlohmann::json result;
    std::exception_ptr eptr;

    boost::asio::co_spawn(ctx, make_query(ctx.get_executor()),
        [&](std::exception_ptr e, nlohmann::json r) {
            eptr = e;
            result = std::move(r);
        });

    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) {
        workers.emplace_back([&ctx] { ctx.run(); });
    }
    for (auto& t : workers) t.join();

    if (eptr) std::rethrow_exception(eptr);
    return result;
}

// Count how many top-level result blocks carry the given "File" meta value
// containing `needle` in their path.
size_t count_files_matching(const nlohmann::json& arr, const std::string& needle) {
    size_t n = 0;
    if (!arr.is_array()) return 0;
    for (const auto& block : arr) {
        if (!block.contains("meta")) continue;
        const auto& fc = block["meta"].value("field_content", nlohmann::json::array());
        if (!fc.empty() && fc[0].is_string() &&
            fc[0].get<std::string>().find(needle) != std::string::npos) {
            ++n;
        }
    }
    return n;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(SearchInterfaceSuite, TreeFixture)

// --- locate_pattern ---------------------------------------------------------

BOOST_AUTO_TEST_CASE(pattern_matches_across_globbed_files) {
    ensure_plugins();

    // "shared_fn" appears in alpha.py and beta.py, not gamma.py.
    auto result = run_query([&](auto ex) -> boost::asio::awaitable<nlohmann::json> {
        SearchInterface search(4, ex);
        co_return co_await search.locate_pattern(root, "**/*.py", "shared_fn");
    });

    BOOST_REQUIRE(result.is_array());
    BOOST_CHECK(count_files_matching(result, "alpha.py") >= 1);
    BOOST_CHECK(count_files_matching(result, "beta.py") >= 1);
    BOOST_CHECK_EQUAL(count_files_matching(result, "gamma.py"), 0u);
}

BOOST_AUTO_TEST_CASE(pattern_glob_scopes_the_search) {
    ensure_plugins();

    // Restrict the glob to pkg/ — only gamma.py should be considered, and it
    // does not contain "shared_fn".
    auto result = run_query([&](auto ex) -> boost::asio::awaitable<nlohmann::json> {
        SearchInterface search(4, ex);
        co_return co_await search.locate_pattern(root, "pkg/*.py", "shared_fn");
    });
    BOOST_REQUIRE(result.is_array());
    BOOST_CHECK(result.empty());

    // But "gamma_only" is there.
    auto hit = run_query([&](auto ex) -> boost::asio::awaitable<nlohmann::json> {
        SearchInterface search(4, ex);
        co_return co_await search.locate_pattern(root, "pkg/*.py", "gamma_only");
    });
    BOOST_CHECK(count_files_matching(hit, "gamma.py") >= 1);
}

BOOST_AUTO_TEST_CASE(pattern_regex_mode) {
    ensure_plugins();

    // Regex: match "return" followed by any chars — present in all .py files.
    auto result = run_query([&](auto ex) -> boost::asio::awaitable<nlohmann::json> {
        SearchInterface search(4, ex);
        co_return co_await search.locate_pattern(root, "**/*.py", "return .*", true);
    });
    BOOST_REQUIRE(result.is_array());
    BOOST_CHECK_GE(result.size(), 3u);
}

BOOST_AUTO_TEST_CASE(non_source_files_are_skipped) {
    ensure_plugins();

    // The glob matches notes.dat, but no plugin claims .dat, so it is skipped
    // silently and yields no results.
    auto result = run_query([&](auto ex) -> boost::asio::awaitable<nlohmann::json> {
        SearchInterface search(4, ex);
        co_return co_await search.locate_pattern(root, "*.dat", "prose");
    });
    BOOST_REQUIRE(result.is_array());
    BOOST_CHECK(result.empty());
}

// --- locate_identifier ------------------------------------------------------

BOOST_AUTO_TEST_CASE(identifier_lookup_across_files) {
    ensure_plugins();

    auto result = run_query([&](auto ex) -> boost::asio::awaitable<nlohmann::json> {
        SearchInterface search(4, ex);
        co_return co_await search.locate_identifier(root, "**/*.py", "shared_fn");
    });
    BOOST_REQUIRE(result.is_array());
    // shared_fn is defined/used in alpha.py and beta.py.
    BOOST_CHECK(count_files_matching(result, "alpha.py") >= 1);
    BOOST_CHECK(count_files_matching(result, "beta.py") >= 1);
}

// --- locate_entity ----------------------------------------------------------

BOOST_AUTO_TEST_CASE(entity_lookup_by_class_name) {
    ensure_plugins();

    // "Alpha" is a class in alpha.py only.
    auto result = run_query([&](auto ex) -> boost::asio::awaitable<nlohmann::json> {
        SearchInterface search(4, ex);
        co_return co_await search.locate_entity(root, "**/*.py", "alpha");
    });
    BOOST_REQUIRE(result.is_array());
    BOOST_CHECK(count_files_matching(result, "alpha.py") >= 1);
    BOOST_CHECK_EQUAL(count_files_matching(result, "beta.py"), 0u);
}

// --- empty / no-match cases -------------------------------------------------

BOOST_AUTO_TEST_CASE(empty_glob_yields_empty_result) {
    ensure_plugins();

    auto result = run_query([&](auto ex) -> boost::asio::awaitable<nlohmann::json> {
        SearchInterface search(4, ex);
        co_return co_await search.locate_pattern(root, "*.nomatch", "anything");
    });
    BOOST_REQUIRE(result.is_array());
    BOOST_CHECK(result.empty());
}

BOOST_AUTO_TEST_CASE(shared_cache_across_facades) {
    ensure_plugins();

    // Two facades over one cache should report identical, non-empty results.
    auto result = run_query([&](auto ex) -> boost::asio::awaitable<nlohmann::json> {
        auto cache = std::make_shared<CacheSystem>(2, ex);
        SearchInterface a(cache);
        SearchInterface b(cache);
        BOOST_CHECK(a.cache() == b.cache());

        auto r1 = co_await a.locate_pattern(root, "**/*.py", "shared_fn");
        auto r2 = co_await b.locate_pattern(root, "**/*.py", "shared_fn");
        BOOST_CHECK_EQUAL(r1.size(), r2.size());
        co_return r1;
    });
    BOOST_CHECK(!result.empty());
}

// --- multi-threaded io_context (concurrent_channel path) --------------------
//
// These pump launch_search through a multi-threaded io_context::run(), the only
// configuration in which the per-slice coroutines can issue concurrent sends on
// the result channel. They guard against regressions to a non-thread-safe
// channel (which would race, drop a batch, or deadlock the collector).

BOOST_AUTO_TEST_CASE(multithreaded_search_merges_every_file) {
    ensure_plugins();

    // Generate many distinct modules so launch_search fans out into several
    // slices, each ending in a channel send potentially from a different worker
    // thread. common_id is defined in every generated file and nowhere else.
    const unsigned N = 24;
    for (unsigned i = 0; i < N; ++i) {
        write("mod_" + std::to_string(i) + ".py",
              "def common_id():\n    return " + std::to_string(i) + "\n");
    }

    auto result = run_query_threaded(4, [&](auto ex) -> boost::asio::awaitable<nlohmann::json> {
        SearchInterface search(8, ex);
        co_return co_await search.locate_identifier(root, "**/*.py", "common_id");
    });

    BOOST_REQUIRE(result.is_array());
    // A lost or hung send would either deadlock run() (test never returns) or
    // drop one module's batch. Assert every module is represented.
    for (unsigned i = 0; i < N; ++i) {
        BOOST_CHECK_GE(count_files_matching(result, "mod_" + std::to_string(i) + ".py"), 1u);
    }
}

BOOST_AUTO_TEST_CASE(multithreaded_matches_singlethreaded_result) {
    ensure_plugins();

    auto threaded = run_query_threaded(4, [&](auto ex) -> boost::asio::awaitable<nlohmann::json> {
        SearchInterface search(4, ex);
        co_return co_await search.locate_pattern(root, "**/*.py", "shared_fn");
    });
    auto single = run_query([&](auto ex) -> boost::asio::awaitable<nlohmann::json> {
        SearchInterface search(4, ex);
        co_return co_await search.locate_pattern(root, "**/*.py", "shared_fn");
    });

    BOOST_REQUIRE(threaded.is_array());
    BOOST_REQUIRE(single.is_array());
    // Same query, same cache width — only the threading model differs. The
    // merged result set must be identical regardless of how many threads pumped
    // the io_context.
    BOOST_CHECK_EQUAL(threaded.size(), single.size());
    BOOST_CHECK_GE(count_files_matching(threaded, "alpha.py"), 1u);
    BOOST_CHECK_GE(count_files_matching(threaded, "beta.py"), 1u);
    BOOST_CHECK_EQUAL(count_files_matching(threaded, "gamma.py"), 0u);
}

BOOST_AUTO_TEST_SUITE_END()
