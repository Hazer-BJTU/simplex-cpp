#define BOOST_TEST_MODULE MemoryArchives
#include <boost/test/unit_test.hpp>
#include "load/archives.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;
namespace {
struct Scratch {
    fs::path root = fs::temp_directory_path()
        / ("simplex_archives_test_" + std::to_string(::getpid()));
    Scratch() {
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    fs::path archive(unsigned int ordinal, std::size_t bytes = 10) {
        auto number = std::to_string(ordinal);
        number.insert(0, 20 - number.size(), '0');
        auto path = root / (number + "-2026-09-27T000000Z-11111111-1111-4111-8111-111111111111");
        fs::create_directory(path);
        std::ofstream(path / "state.md") << std::string(bytes, 'x');
        return path;
    }
};
}

BOOST_AUTO_TEST_CASE(count_limit_keeps_current_and_newest) {
    Scratch test;
    const auto first = test.archive(1);
    const auto second = test.archive(2);
    const auto current = test.archive(3);
    auto result = load::prune_memory_archives(test.root, current, {2, 0, 0});
    BOOST_TEST(result.removed_archives == 1u);
    BOOST_TEST(result.removed_bytes == 10u);
    BOOST_CHECK(!fs::exists(first));
    BOOST_CHECK(fs::exists(second));
    BOOST_CHECK(fs::exists(current));
}

BOOST_AUTO_TEST_CASE(byte_and_age_limits_never_delete_current) {
    Scratch test;
    const auto old = test.archive(1);
    const auto expired = test.archive(2);
    const auto current = test.archive(3, 100);
    fs::last_write_time(expired / "state.md",
        fs::file_time_type::clock::now() - std::chrono::hours(48));
    auto result = load::prune_memory_archives(test.root, current, {0, 0, 1});
    BOOST_TEST(result.removed_archives == 1u);
    BOOST_CHECK(fs::exists(old));
    result = load::prune_memory_archives(test.root, current, {0, 1, 0});
    BOOST_TEST(result.removed_archives == 1u);
    BOOST_CHECK(fs::exists(current / "state.md"));
}

BOOST_AUTO_TEST_CASE(disabled_limits_and_unrecognized_contents_are_preserved) {
    Scratch test;
    const auto unknown = test.archive(1);
    std::ofstream(unknown / "notes.txt") << "operator data";
    const auto linked = test.archive(2);
    fs::remove(linked / "state.md");
    fs::create_symlink(unknown / "notes.txt", linked / "state.md");
    const auto regular = test.archive(3);
    const auto current = test.archive(4);
    BOOST_TEST(load::prune_memory_archives(test.root, current, {0, 0, 0}).removed_archives == 0u);
    BOOST_TEST(load::prune_memory_archives(test.root, current, {1, 0, 0}).removed_archives == 1u);
    BOOST_CHECK(!fs::exists(regular));
    BOOST_CHECK(fs::exists(unknown / "notes.txt"));
    BOOST_CHECK(fs::is_symlink(linked / "state.md"));
    BOOST_CHECK_THROW(load::prune_memory_archives(test.root, test.root / "missing", {1, 0, 0}),
        std::runtime_error);
    BOOST_CHECK(fs::exists(current));
}
