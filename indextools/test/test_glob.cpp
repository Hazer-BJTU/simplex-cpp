#define BOOST_TEST_MODULE GlobTests
#include <boost/test/unit_test.hpp>

#include "indextools/utils.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace indextools;
namespace fs = std::filesystem;

// ============================================================================
// Test fixture: creates a temporary directory tree and cleans it up
// ============================================================================

struct TempDirFixture {
    fs::path root;

    TempDirFixture() {
        // Create unique temp directory
        root = fs::temp_directory_path() / ("indextools_glob_test_" +
               std::to_string(std::hash<std::string>{}("indextools_glob_test") ^
                              std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);

        // Build test directory tree:
        //
        // root/
        // ├── file1.cpp
        // ├── file2.cpp
        // ├── readme.txt
        // ├── .hidden.txt
        // ├── sub1/
        // │   ├── test_a.cpp
        // │   ├── test_b.cpp
        // │   ├── other.txt
        // │   └── nested/
        // │       ├── deep.cpp
        // │       └── data.json
        // ├── sub2/
        // │   ├── file3.cpp
        // │   └── readme.md
        // └── empty_dir/

        create_file("file1.cpp",    "// file1");
        create_file("file2.cpp",    "// file2");
        create_file("readme.txt",   "readme");
        create_file(".hidden.txt",  "hidden");

        create_dir("sub1");
        create_file("sub1/test_a.cpp", "// test_a");
        create_file("sub1/test_b.cpp", "// test_b");
        create_file("sub1/other.txt",  "other");

        create_dir("sub1/nested");
        create_file("sub1/nested/deep.cpp", "// deep");
        create_file("sub1/nested/data.json", "{}");

        create_dir("sub2");
        create_file("sub2/file3.cpp", "// file3");
        create_file("sub2/readme.md", "# readme");

        create_dir("empty_dir");
    }

    ~TempDirFixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void create_file(const std::string& rel_path, const std::string& content) {
        fs::path full = root / rel_path;
        fs::create_directories(full.parent_path());
        std::ofstream ofs(full);
        ofs << content;
        if (!ofs) {
            throw std::runtime_error("Failed to create test file: " + full.string());
        }
    }

    void create_dir(const std::string& rel_path) {
        fs::create_directories(root / rel_path);
    }

    // Helper: get absolute path of a relative path within the temp dir
    std::string abs_path(const std::string& rel_path) const {
        return (root / rel_path).string();
    }

    // Helper: run glob_find and return a set of strings for easy comparison.
    // glob_find now returns std::filesystem::path objects; convert each to its
    // string form so the expected-value sets below stay plain strings.
    std::set<std::string> glob_set(const std::string& pattern) {
        auto vec = glob_find(root, pattern);
        std::set<std::string> out;
        for (const auto& p : vec) {
            out.insert(p.string());
        }
        return out;
    }
};

// ============================================================================
// Suite: GlobMatchSuite — unit tests for glob_match()
// ============================================================================

BOOST_AUTO_TEST_SUITE(GlobMatchSuite)

BOOST_AUTO_TEST_CASE(exact_match)
{
    BOOST_CHECK(glob_match("hello", "hello"));
    BOOST_CHECK(!glob_match("hello", "world"));
    BOOST_CHECK(!glob_match("hello", "helloo"));
    BOOST_CHECK(!glob_match("hello", "hell"));
}

BOOST_AUTO_TEST_CASE(empty_pattern_and_name)
{
    BOOST_CHECK(glob_match("", ""));
    BOOST_CHECK(!glob_match("", "a"));
}

BOOST_AUTO_TEST_CASE(star_only)
{
    // * matches everything
    BOOST_CHECK(glob_match("*", ""));
    BOOST_CHECK(glob_match("*", "a"));
    BOOST_CHECK(glob_match("*", "hello_world"));
    BOOST_CHECK(glob_match("*", "file.cpp"));
}

BOOST_AUTO_TEST_CASE(star_prefix)
{
    // * at the beginning
    BOOST_CHECK(glob_match("*.cpp", "file.cpp"));
    BOOST_CHECK(glob_match("*.cpp", ".cpp"));        // * can match zero chars
    BOOST_CHECK(!glob_match("*.cpp", "file.cp"));
    BOOST_CHECK(!glob_match("*.cpp", "file.cppp"));
    BOOST_CHECK(!glob_match("*.cpp", "file.hpp"));
}

BOOST_AUTO_TEST_CASE(star_suffix)
{
    // * at the end
    BOOST_CHECK(glob_match("file*", "file"));
    BOOST_CHECK(glob_match("file*", "file.cpp"));
    BOOST_CHECK(glob_match("file*", "file_something_else"));
    BOOST_CHECK(!glob_match("file*", "fil.cpp"));
    BOOST_CHECK(!glob_match("file*", "other"));
}

BOOST_AUTO_TEST_CASE(star_middle)
{
    // * in the middle
    BOOST_CHECK(glob_match("test_*_test", "test_foo_test"));
    BOOST_CHECK(glob_match("test_*_test", "test__test"));    // zero chars
    BOOST_CHECK(glob_match("test_*_test", "test_abc_def_ghi_test"));
    BOOST_CHECK(!glob_match("test_*_test", "test_foo_tes"));
    BOOST_CHECK(!glob_match("test_*_test", "test_foo_testt"));
}

BOOST_AUTO_TEST_CASE(multiple_stars)
{
    BOOST_CHECK(glob_match("a*b*c", "abc"));
    BOOST_CHECK(glob_match("a*b*c", "aXbYc"));
    BOOST_CHECK(glob_match("a*b*c", "aXXbYYc"));
    BOOST_CHECK(!glob_match("a*b*c", "acb"));
    BOOST_CHECK(!glob_match("a*b*c", "ab"));
}

BOOST_AUTO_TEST_CASE(consecutive_stars)
{
    // ** in a single segment == *
    BOOST_CHECK(glob_match("a**b", "ab"));
    BOOST_CHECK(glob_match("a**b", "aXYZb"));
    BOOST_CHECK(glob_match("a**b", "acb"));   // * matches "c"
    BOOST_CHECK(!glob_match("a**b", "a"));    // needs trailing 'b'
    BOOST_CHECK(!glob_match("a**b", "b"));    // needs leading 'a'
}

BOOST_AUTO_TEST_CASE(question_mark)
{
    // ? matches exactly one character
    BOOST_CHECK(glob_match("file?.cpp", "file1.cpp"));
    BOOST_CHECK(glob_match("file?.cpp", "file_.cpp"));
    BOOST_CHECK(glob_match("file?.cpp", "filea.cpp"));
    BOOST_CHECK(!glob_match("file?.cpp", "file10.cpp"));   // two chars
    BOOST_CHECK(!glob_match("file?.cpp", "file.cpp"));      // zero chars
    BOOST_CHECK(glob_match("????", "abcd"));
    BOOST_CHECK(!glob_match("????", "abc"));
    BOOST_CHECK(!glob_match("????", "abcde"));
}

BOOST_AUTO_TEST_CASE(star_and_question)
{
    BOOST_CHECK(glob_match("file?.*", "file1.cpp"));
    BOOST_CHECK(glob_match("file?.*", "file1."));
    BOOST_CHECK(!glob_match("file?.*", "file.cpp"));        // ? needs one char
    BOOST_CHECK(!glob_match("file?.*", "file12.cpp"));       // ? needs exactly one char
}

BOOST_AUTO_TEST_CASE(character_class_simple)
{
    // [abc] matches a, b, or c
    BOOST_CHECK(glob_match("file[abc].cpp", "filea.cpp"));
    BOOST_CHECK(glob_match("file[abc].cpp", "fileb.cpp"));
    BOOST_CHECK(glob_match("file[abc].cpp", "filec.cpp"));
    BOOST_CHECK(!glob_match("file[abc].cpp", "filed.cpp"));
    BOOST_CHECK(!glob_match("file[abc].cpp", "fileab.cpp"));
}

BOOST_AUTO_TEST_CASE(character_class_range)
{
    // [0-9] matches digits
    BOOST_CHECK(glob_match("file[0-9].cpp", "file0.cpp"));
    BOOST_CHECK(glob_match("file[0-9].cpp", "file5.cpp"));
    BOOST_CHECK(glob_match("file[0-9].cpp", "file9.cpp"));
    BOOST_CHECK(!glob_match("file[0-9].cpp", "filea.cpp"));
    BOOST_CHECK(!glob_match("file[0-9].cpp", "file10.cpp"));

    // [a-z] matches lowercase letters
    BOOST_CHECK(glob_match("[a-z]at", "cat"));
    BOOST_CHECK(glob_match("[a-z]at", "hat"));
    BOOST_CHECK(!glob_match("[a-z]at", "Cat"));
    BOOST_CHECK(!glob_match("[a-z]at", "1at"));
}

BOOST_AUTO_TEST_CASE(character_class_negation)
{
    // [!abc] matches anything except a, b, c
    BOOST_CHECK(glob_match("file[!abc].cpp", "filed.cpp"));
    BOOST_CHECK(glob_match("file[!abc].cpp", "filex.cpp"));
    BOOST_CHECK(!glob_match("file[!abc].cpp", "filea.cpp"));
    BOOST_CHECK(!glob_match("file[!abc].cpp", "fileb.cpp"));

    // [^abc] also negates
    BOOST_CHECK(glob_match("file[^abc].cpp", "filed.cpp"));
    BOOST_CHECK(!glob_match("file[^abc].cpp", "filea.cpp"));
}

BOOST_AUTO_TEST_CASE(character_class_edge_cases)
{
    // ']' as first char in class: literal ']'
    BOOST_CHECK(glob_match("file[]abc].cpp", "file].cpp"));
    BOOST_CHECK(glob_match("file[]abc].cpp", "filea.cpp"));
    BOOST_CHECK(!glob_match("file[]abc].cpp", "filed.cpp"));

    // '-' as first char in class: literal '-'
    BOOST_CHECK(glob_match("file[-abc].cpp", "file-.cpp"));
    BOOST_CHECK(glob_match("file[-abc].cpp", "filea.cpp"));

    // '-' as last char in class: literal '-'
    BOOST_CHECK(glob_match("file[abc-].cpp", "file-.cpp"));
    BOOST_CHECK(glob_match("file[abc-].cpp", "filea.cpp"));
}

BOOST_AUTO_TEST_CASE(unmatched_bracket_treated_as_literal)
{
    // Unmatched '[' — treated as literal character
    BOOST_CHECK(glob_match("file[.cpp", "file[.cpp"));
    BOOST_CHECK(!glob_match("file[.cpp", "filea.cpp"));
    BOOST_CHECK(glob_match("file[a.cpp", "file[a.cpp"));
}

BOOST_AUTO_TEST_CASE(terminal_bracket_treated_as_literal)
{
    // Terminal '[' (last char of pattern) — treated as literal
    BOOST_CHECK(glob_match("[", "["));
    BOOST_CHECK(!glob_match("[", "a"));
    BOOST_CHECK(glob_match("file[", "file["));
    BOOST_CHECK(!glob_match("file[", "filea"));
    BOOST_CHECK(glob_match("*[", "abc["));
    BOOST_CHECK(!glob_match("*[", "abc"));
}

BOOST_AUTO_TEST_CASE(case_sensitivity)
{
    // glob_match is case-sensitive by default
    BOOST_CHECK(glob_match("Hello", "Hello"));
    BOOST_CHECK(!glob_match("Hello", "hello"));
    BOOST_CHECK(!glob_match("hello", "HELLO"));
}

BOOST_AUTO_TEST_CASE(complex_patterns)
{
    // Various realistic filename patterns
    BOOST_CHECK(glob_match("test_*.cpp", "test_utils.cpp"));
    BOOST_CHECK(glob_match("test_*.cpp", "test_glob.cpp"));
    BOOST_CHECK(!glob_match("test_*.cpp", "test_utils.h"));

    BOOST_CHECK(glob_match("*.tar.gz", "archive.tar.gz"));
    BOOST_CHECK(!glob_match("*.tar.gz", "archive.tar"));
    BOOST_CHECK(!glob_match("*.tar.gz", "archive.zip"));

    BOOST_CHECK(glob_match("DSC_[0-9][0-9][0-9][0-9].jpg", "DSC_0001.jpg"));
    BOOST_CHECK(glob_match("DSC_[0-9][0-9][0-9][0-9].jpg", "DSC_9999.jpg"));
    BOOST_CHECK(!glob_match("DSC_[0-9][0-9][0-9][0-9].jpg", "DSC_00001.jpg"));
}

BOOST_AUTO_TEST_CASE(star_backtracks_correctly)
{
    // Test that * backtracks correctly (non-greedy matching)
    // Pattern: a*bc — "abc" should NOT match because * consumes nothing
    // leaving "bc" to match "bc"? Actually "a*bc": a + * matches "" + bc matches "bc"
    // That does match. Hmm.
    //
    // Let me think of a better test case:
    // Pattern: a*ba — "aba" → a + * = "b" + ba? No. a + * = "" + ba matches "ba"? No, "ba" != "aba"
    // Wait: a*ba against "aba":
    //   a matches a, *ba against "ba":
    //     * matches "", ba against "ba": b matches b, a matches a → YES
    // OK that matches.
    //
    // Pattern: *a*b against "axb": * matches "", a matches a, * matches "x", b matches b → YES
    //
    // Pattern: a*a*b against "aab":
    //   a matches a, *a*b against "ab":
    //     * matches "", a matches a, *b against "b": * matches "", b matches b → YES
    //
    // Classic backtrack test: *a*a against "baaa"
    //   * matches "", a against "b"? No. * matches "b", a against "a"? Yes. *a against "aa":
    //     * matches "", a against "a"? Yes. a against "a"? Matched → YES

    BOOST_CHECK(glob_match("*a*a", "baaa"));
    BOOST_CHECK(glob_match("*a*b", "aaaaab"));
    BOOST_CHECK(glob_match("*a*b*", "xyzaabxyz"));
    BOOST_CHECK(!glob_match("*a*b", "xyz"));  // no 'a' then 'b'
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: GlobFindSuite — integration tests for glob_find()
// ============================================================================

BOOST_FIXTURE_TEST_SUITE(GlobFindSuite, TempDirFixture)

BOOST_AUTO_TEST_CASE(empty_pattern_returns_empty)
{
    auto result = glob_find(root, "");
    BOOST_CHECK(result.empty());
}

BOOST_AUTO_TEST_CASE(nonexistent_root_returns_empty)
{
    auto result = glob_find(root / "nonexistent_dir_xyz", "*.cpp");
    BOOST_CHECK(result.empty());
}

BOOST_AUTO_TEST_CASE(single_segment_star_cpp)
{
    // *.cpp — only root-level .cpp files
    auto s = glob_set("*.cpp");
    std::set<std::string> expected = {
        abs_path("file1.cpp"),
        abs_path("file2.cpp"),
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(s.begin(), s.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(single_segment_star_txt)
{
    // *.txt — root-level .txt files (not in subdirs).
    // Note: * matches leading dots, so .hidden.txt is included.
    auto s = glob_set("*.txt");
    std::set<std::string> expected = {
        abs_path(".hidden.txt"),
        abs_path("readme.txt"),
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(s.begin(), s.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(double_star_all_cpp)
{
    // **/*.cpp — all .cpp files at any depth
    auto s = glob_set("**/*.cpp");
    std::set<std::string> expected = {
        abs_path("file1.cpp"),
        abs_path("file2.cpp"),
        abs_path("sub1/test_a.cpp"),
        abs_path("sub1/test_b.cpp"),
        abs_path("sub1/nested/deep.cpp"),
        abs_path("sub2/file3.cpp"),
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(s.begin(), s.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(double_star_all_txt)
{
    // **/*.txt — all .txt files recursively.
    // Note: * matches leading dots, so .hidden.txt is included.
    auto s = glob_set("**/*.txt");
    std::set<std::string> expected = {
        abs_path(".hidden.txt"),
        abs_path("readme.txt"),
        abs_path("sub1/other.txt"),
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(s.begin(), s.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(double_star_only_all_files)
{
    // ** — all files at all depths
    auto s = glob_set("**");
    std::set<std::string> expected = {
        abs_path("file1.cpp"),
        abs_path("file2.cpp"),
        abs_path("readme.txt"),
        abs_path(".hidden.txt"),
        abs_path("sub1/test_a.cpp"),
        abs_path("sub1/test_b.cpp"),
        abs_path("sub1/other.txt"),
        abs_path("sub1/nested/deep.cpp"),
        abs_path("sub1/nested/data.json"),
        abs_path("sub2/file3.cpp"),
        abs_path("sub2/readme.md"),
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(s.begin(), s.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(specific_subdir)
{
    // sub1/*.cpp — only .cpp files directly in sub1/
    auto s = glob_set("sub1/*.cpp");
    std::set<std::string> expected = {
        abs_path("sub1/test_a.cpp"),
        abs_path("sub1/test_b.cpp"),
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(s.begin(), s.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(specific_subdir_txt)
{
    // sub1/*.txt
    auto s = glob_set("sub1/*.txt");
    std::set<std::string> expected = {
        abs_path("sub1/other.txt"),
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(s.begin(), s.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(double_star_in_middle)
{
    // sub1/**/deep.cpp — deep.cpp at any depth under sub1/
    auto s = glob_set("sub1/**/deep.cpp");
    std::set<std::string> expected = {
        abs_path("sub1/nested/deep.cpp"),
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(s.begin(), s.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(double_star_followed_by_pattern)
{
    // **/deep.* — files named "deep" with any extension at any depth
    auto s = glob_set("**/deep.*");
    std::set<std::string> expected = {
        abs_path("sub1/nested/deep.cpp"),
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(s.begin(), s.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(double_star_middle_and_pattern)
{
    // sub1/**/*.json — all .json files under sub1/
    auto s = glob_set("sub1/**/*.json");
    std::set<std::string> expected = {
        abs_path("sub1/nested/data.json"),
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(s.begin(), s.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(question_mark_in_filename)
{
    // sub1/test_?.cpp — test_a.cpp, test_b.cpp (single char after test_)
    auto s = glob_set("sub1/test_?.cpp");
    std::set<std::string> expected = {
        abs_path("sub1/test_a.cpp"),
        abs_path("sub1/test_b.cpp"),
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(s.begin(), s.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(question_mark_in_path)
{
    // sub?/file3.cpp — sub1 or sub2 (single char)
    // Note: this depends on question mark matching in directory names.
    // Actually, let's make a cleaner test.
    auto s = glob_set("sub?/file3.cpp");
    std::set<std::string> expected = {
        abs_path("sub2/file3.cpp"),
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(s.begin(), s.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(character_class_in_filename)
{
    // sub?/file[0-9].cpp — file3.cpp (file followed by a single digit)
    auto s = glob_set("sub?/file[0-9].cpp");
    std::set<std::string> expected = {
        abs_path("sub2/file3.cpp"),
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(s.begin(), s.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(pruning_nonexistent_subdir)
{
    // no_match/*.cpp — directory "no_match" doesn't exist
    // The traversal should prune immediately (no directory matching "no_match")
    auto s = glob_set("no_match/*.cpp");
    BOOST_CHECK(s.empty());
}

BOOST_AUTO_TEST_CASE(pruning_unmatched_intermediate_dir)
{
    // sub1/nomatch/*.cpp — sub1 exists, but "nomatch" doesn't
    auto s = glob_set("sub1/nomatch/*.cpp");
    BOOST_CHECK(s.empty());
}

BOOST_AUTO_TEST_CASE(exact_filename)
{
    auto s = glob_set("readme.txt");
    std::set<std::string> expected = {
        abs_path("readme.txt"),
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(s.begin(), s.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(hidden_file_glob)
{
    // * does match leading dot (standard glob behavior)
    // .hidden.txt is matched by *.txt
    auto s = glob_set("*.txt");
    BOOST_CHECK(s.count(abs_path(".hidden.txt")) == 1);

    // Explicitly match hidden files
    auto s2 = glob_set(".*.txt");
    std::set<std::string> expected = {
        abs_path(".hidden.txt"),
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(s2.begin(), s2.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(empty_directory_handled_gracefully)
{
    // empty_dir/ exists but has no files
    auto s = glob_set("empty_dir/*");
    BOOST_CHECK(s.empty());
}

BOOST_AUTO_TEST_CASE(pattern_with_leading_dot_slash)
{
    // ./file*.cpp should work the same as file*.cpp
    auto s = glob_set("./file*.cpp");
    std::set<std::string> expected = {
        abs_path("file1.cpp"),
        abs_path("file2.cpp"),
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(s.begin(), s.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(pattern_with_dotdot_segments)
{
    // ".." segments are filtered out, so sub1/../sub2/*.cpp == sub2/*.cpp
    auto s1 = glob_set("sub1/../sub2/*.cpp");
    auto s2 = glob_set("sub2/*.cpp");
    BOOST_CHECK_EQUAL_COLLECTIONS(s1.begin(), s1.end(), s2.begin(), s2.end());
    BOOST_CHECK(!s1.empty());
}

BOOST_AUTO_TEST_CASE(double_star_with_suffix_only)
{
    // **.cpp — not a valid pattern really, but "**" is a segment and ".cpp"
    // would be... wait, "**.cpp" is a single segment. Let's test it.
    // "**.cpp" would need "**" to match as a glob within a single filename.
    // But ** in a single segment is just * (consecutive stars collapse).
    // So "**.cpp" == "*.cpp" in glob_match terms, but in glob_find the segment
    // is "**.cpp" which is NOT the special "**" segment.
    // So glob_find("**.cpp") matches files named like "<anything>.cpp" at root.
    auto s = glob_set("**.cpp");
    std::set<std::string> expected = {
        abs_path("file1.cpp"),
        abs_path("file2.cpp"),
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(s.begin(), s.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(result_is_sorted)
{
    auto vec = glob_find(root, "**/*.cpp");
    BOOST_CHECK(std::is_sorted(vec.begin(), vec.end()));
}

BOOST_AUTO_TEST_CASE(abs_paths_returned)
{
    auto vec = glob_find(root, "file1.cpp");
    BOOST_REQUIRE_EQUAL(vec.size(), 1u);
    // glob_find returns std::filesystem::path objects (not strings).
    static_assert(std::is_same_v<decltype(vec)::value_type, fs::path>);
    BOOST_CHECK(vec[0].is_absolute());
}

BOOST_AUTO_TEST_CASE(no_match_pattern_returns_empty)
{
    auto s = glob_set("*.py");
    BOOST_CHECK(s.empty());
}

BOOST_AUTO_TEST_CASE(trailing_slash_in_pattern)
{
    // "sub1/" should be equivalent to "sub1/*"? No — "sub1/" splits to {"sub1"}.
    // The last segment "sub1" matches a directory. Since it's the last segment,
    // we only check for regular files, so nothing matches.
    // Let's just verify it doesn't crash and returns something reasonable.
    auto vec = glob_find(root, "sub1/");
    // "sub1" is a directory, not a file, so result should be empty
    BOOST_CHECK(vec.empty());
}

BOOST_AUTO_TEST_CASE(double_star_at_end_matches_all_under)
{
    // sub1/** — all files under sub1/
    auto s = glob_set("sub1/**");
    std::set<std::string> expected = {
        abs_path("sub1/test_a.cpp"),
        abs_path("sub1/test_b.cpp"),
        abs_path("sub1/other.txt"),
        abs_path("sub1/nested/deep.cpp"),
        abs_path("sub1/nested/data.json"),
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(s.begin(), s.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(mixed_stars_and_dirs)
{
    // **/sub1/*.cpp — all .cpp files directly in any sub1/ directory
    auto s = glob_set("**/sub1/*.cpp");
    std::set<std::string> expected = {
        abs_path("sub1/test_a.cpp"),
        abs_path("sub1/test_b.cpp"),
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(s.begin(), s.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(test_cpp_prefix_glob)
{
    // sub1/test_*.cpp
    auto s = glob_set("sub1/test_*.cpp");
    std::set<std::string> expected = {
        abs_path("sub1/test_a.cpp"),
        abs_path("sub1/test_b.cpp"),
    };
    BOOST_CHECK_EQUAL_COLLECTIONS(s.begin(), s.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_SUITE_END()
