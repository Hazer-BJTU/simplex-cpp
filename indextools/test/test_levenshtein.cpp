#define BOOST_TEST_MODULE LevenshteinTests
#include <boost/test/unit_test.hpp>

#include "indextools/editor.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <algorithm>

using namespace indextools;

// ============================================================================
// Helpers
// ============================================================================
//
// The current levenshtein_distance() is a Myers diff:
//   - returns std::optional<size_t> = length of the shortest edit script D
//     (number of single-element deletions + insertions; a "substitution" is
//     modelled as one deletion + one insertion, so it costs 2).
//   - pos_deleted holds ascending indices into the ORIGINAL sequence.
//   - pos_added   holds ascending indices into the MODIFIED sequence.
//   - D == pos_deleted.size() + pos_added.size().

namespace {

template <typename T>
auto make_equal(const std::vector<T>& a, const std::vector<T>& b) {
    return [&a, &b](size_t i, size_t j) -> bool { return a[i] == b[j]; };
}

/// Assert the invariant that D equals the total number of recorded edits and
/// that both index lists are strictly ascending and in range.
void check_invariants(const std::optional<size_t>& d,
                      const std::vector<size_t>& deleted,
                      const std::vector<size_t>& added,
                      size_t M, size_t N) {
    BOOST_REQUIRE(d.has_value());
    BOOST_CHECK_EQUAL(*d, deleted.size() + added.size());
    for (size_t i = 1; i < deleted.size(); ++i) {
        BOOST_CHECK_LT(deleted[i - 1], deleted[i]);
    }
    for (size_t j = 1; j < added.size(); ++j) {
        BOOST_CHECK_LT(added[j - 1], added[j]);
    }
    if (!deleted.empty()) BOOST_CHECK_LT(deleted.back(), M);
    if (!added.empty())   BOOST_CHECK_LT(added.back(), N);
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(LevenshteinDistanceSuite)

// --- Edge cases --------------------------------------------------------------

BOOST_AUTO_TEST_CASE(both_empty_returns_zero)
{
    std::vector<int> a, b;
    std::vector<size_t> deleted, added;
    auto d = levenshtein_distance(make_equal(a, b), a.size(), b.size(), deleted, added);
    BOOST_REQUIRE(d.has_value());
    BOOST_CHECK_EQUAL(*d, 0u);
    BOOST_CHECK(deleted.empty());
    BOOST_CHECK(added.empty());
}

BOOST_AUTO_TEST_CASE(original_empty_all_added)
{
    std::vector<int> a, b{1, 2, 3};
    std::vector<size_t> deleted, added;
    auto d = levenshtein_distance(make_equal(a, b), a.size(), b.size(), deleted, added);
    BOOST_REQUIRE(d.has_value());
    BOOST_CHECK_EQUAL(*d, 3u);
    BOOST_CHECK(deleted.empty());
    BOOST_REQUIRE_EQUAL(added.size(), 3u);
    BOOST_CHECK_EQUAL(added[0], 0u);
    BOOST_CHECK_EQUAL(added[1], 1u);
    BOOST_CHECK_EQUAL(added[2], 2u);
}

BOOST_AUTO_TEST_CASE(modified_empty_all_deleted)
{
    std::vector<int> a{1, 2, 3}, b;
    std::vector<size_t> deleted, added;
    auto d = levenshtein_distance(make_equal(a, b), a.size(), b.size(), deleted, added);
    BOOST_REQUIRE(d.has_value());
    BOOST_CHECK_EQUAL(*d, 3u);
    BOOST_REQUIRE_EQUAL(deleted.size(), 3u);
    BOOST_CHECK_EQUAL(deleted[0], 0u);
    BOOST_CHECK_EQUAL(deleted[1], 1u);
    BOOST_CHECK_EQUAL(deleted[2], 2u);
    BOOST_CHECK(added.empty());
}

// --- Identical sequences -----------------------------------------------------

BOOST_AUTO_TEST_CASE(identical_sequences_zero_distance)
{
    std::vector<int> a{1, 2, 3, 4, 5}, b{1, 2, 3, 4, 5};
    std::vector<size_t> deleted, added;
    auto d = levenshtein_distance(make_equal(a, b), a.size(), b.size(), deleted, added);
    BOOST_REQUIRE(d.has_value());
    BOOST_CHECK_EQUAL(*d, 0u);
    BOOST_CHECK(deleted.empty());
    BOOST_CHECK(added.empty());
}

BOOST_AUTO_TEST_CASE(single_element_same)
{
    std::vector<int> a{42}, b{42};
    std::vector<size_t> deleted, added;
    auto d = levenshtein_distance(make_equal(a, b), a.size(), b.size(), deleted, added);
    BOOST_REQUIRE(d.has_value());
    BOOST_CHECK_EQUAL(*d, 0u);
    BOOST_CHECK(deleted.empty());
    BOOST_CHECK(added.empty());
}

// --- Single operations -------------------------------------------------------

BOOST_AUTO_TEST_CASE(single_deletion)
{
    std::vector<int> a{1, 2, 3}, b{1, 3};
    std::vector<size_t> deleted, added;
    auto d = levenshtein_distance(make_equal(a, b), a.size(), b.size(), deleted, added);
    check_invariants(d, deleted, added, a.size(), b.size());
    BOOST_CHECK_EQUAL(*d, 1u);
    BOOST_REQUIRE_EQUAL(deleted.size(), 1u);
    BOOST_CHECK_EQUAL(deleted[0], 1u);  // "2" deleted
    BOOST_CHECK(added.empty());
}

BOOST_AUTO_TEST_CASE(single_insertion)
{
    std::vector<int> a{1, 3}, b{1, 2, 3};
    std::vector<size_t> deleted, added;
    auto d = levenshtein_distance(make_equal(a, b), a.size(), b.size(), deleted, added);
    check_invariants(d, deleted, added, a.size(), b.size());
    BOOST_CHECK_EQUAL(*d, 1u);
    BOOST_CHECK(deleted.empty());
    BOOST_REQUIRE_EQUAL(added.size(), 1u);
    BOOST_CHECK_EQUAL(added[0], 1u);  // "2" inserted
}

BOOST_AUTO_TEST_CASE(single_substitution_costs_two)
{
    // In an edit-script (Myers) model a substitution is one delete + one insert.
    std::vector<int> a{1, 2, 3}, b{1, 9, 3};
    std::vector<size_t> deleted, added;
    auto d = levenshtein_distance(make_equal(a, b), a.size(), b.size(), deleted, added);
    check_invariants(d, deleted, added, a.size(), b.size());
    BOOST_CHECK_EQUAL(*d, 2u);
    BOOST_REQUIRE_EQUAL(deleted.size(), 1u);
    BOOST_REQUIRE_EQUAL(added.size(), 1u);
    BOOST_CHECK_EQUAL(deleted[0], 1u);
    BOOST_CHECK_EQUAL(added[0], 1u);
}

// --- Complex operations ------------------------------------------------------

BOOST_AUTO_TEST_CASE(prefix_deletion)
{
    std::vector<int> a{10, 20, 1, 2, 3}, b{1, 2, 3};
    std::vector<size_t> deleted, added;
    auto d = levenshtein_distance(make_equal(a, b), a.size(), b.size(), deleted, added);
    check_invariants(d, deleted, added, a.size(), b.size());
    BOOST_CHECK_EQUAL(*d, 2u);
    BOOST_REQUIRE_EQUAL(deleted.size(), 2u);
    BOOST_CHECK_EQUAL(deleted[0], 0u);
    BOOST_CHECK_EQUAL(deleted[1], 1u);
    BOOST_CHECK(added.empty());
}

BOOST_AUTO_TEST_CASE(suffix_insertion)
{
    std::vector<int> a{1, 2, 3}, b{1, 2, 3, 4, 5};
    std::vector<size_t> deleted, added;
    auto d = levenshtein_distance(make_equal(a, b), a.size(), b.size(), deleted, added);
    check_invariants(d, deleted, added, a.size(), b.size());
    BOOST_CHECK_EQUAL(*d, 2u);
    BOOST_CHECK(deleted.empty());
    BOOST_REQUIRE_EQUAL(added.size(), 2u);
    BOOST_CHECK_EQUAL(added[0], 3u);
    BOOST_CHECK_EQUAL(added[1], 4u);
}

BOOST_AUTO_TEST_CASE(multiple_substitutions)
{
    // a=[1,2,3,4] -> b=[1,8,3,9]: two substitutions => D = 4.
    std::vector<int> a{1, 2, 3, 4}, b{1, 8, 3, 9};
    std::vector<size_t> deleted, added;
    auto d = levenshtein_distance(make_equal(a, b), a.size(), b.size(), deleted, added);
    check_invariants(d, deleted, added, a.size(), b.size());
    BOOST_CHECK_EQUAL(*d, 4u);
    BOOST_REQUIRE_EQUAL(deleted.size(), 2u);
    BOOST_REQUIRE_EQUAL(added.size(), 2u);
    BOOST_CHECK_EQUAL(deleted[0], 1u);
    BOOST_CHECK_EQUAL(deleted[1], 3u);
    BOOST_CHECK_EQUAL(added[0], 1u);
    BOOST_CHECK_EQUAL(added[1], 3u);
}

BOOST_AUTO_TEST_CASE(completely_different_sequences)
{
    // No shared elements: every original deleted, every modified inserted.
    std::vector<int> a{10, 20, 30}, b{40, 50, 60, 70};
    std::vector<size_t> deleted, added;
    auto d = levenshtein_distance(make_equal(a, b), a.size(), b.size(), deleted, added);
    check_invariants(d, deleted, added, a.size(), b.size());
    BOOST_CHECK_EQUAL(*d, 7u);  // 3 deletes + 4 inserts
    BOOST_CHECK_EQUAL(deleted.size(), 3u);
    BOOST_CHECK_EQUAL(added.size(), 4u);
}

// --- String-based (line-level diff use case) ---------------------------------

BOOST_AUTO_TEST_CASE(line_diff_identical_files)
{
    std::vector<std::string> original{"def foo():", "    x = 1", "    return x"};
    std::vector<std::string> modified = original;
    std::vector<size_t> deleted, added;
    auto d = levenshtein_distance(make_equal(original, modified),
                                  original.size(), modified.size(), deleted, added);
    BOOST_REQUIRE(d.has_value());
    BOOST_CHECK_EQUAL(*d, 0u);
    BOOST_CHECK(deleted.empty());
    BOOST_CHECK(added.empty());
}

BOOST_AUTO_TEST_CASE(line_diff_deleted_lines)
{
    std::vector<std::string> original{"def foo():", "    # TODO: implement", "    pass"};
    std::vector<std::string> modified{"def foo():", "    pass"};
    std::vector<size_t> deleted, added;
    auto d = levenshtein_distance(make_equal(original, modified),
                                  original.size(), modified.size(), deleted, added);
    check_invariants(d, deleted, added, original.size(), modified.size());
    BOOST_CHECK_EQUAL(*d, 1u);
    BOOST_REQUIRE_EQUAL(deleted.size(), 1u);
    BOOST_CHECK_EQUAL(deleted[0], 1u);
    BOOST_CHECK(added.empty());
}

BOOST_AUTO_TEST_CASE(line_diff_shared_lines_are_not_marked)
{
    // The unchanged header and trailing lines must stay off both lists.
    std::vector<std::string> original{
        "#include <stdio.h>", "", "int main() {", "    return 0;", "}"};
    std::vector<std::string> modified{
        "#include <stdio.h>", "#include <stdlib.h>", "", "int main() {",
        "    return EXIT_SUCCESS;", "}"};
    std::vector<size_t> deleted, added;
    auto d = levenshtein_distance(make_equal(original, modified),
                                  original.size(), modified.size(), deleted, added);
    check_invariants(d, deleted, added, original.size(), modified.size());
    // Header (orig 0 / mod 0) and closing brace are shared -> not recorded.
    BOOST_CHECK(std::find(deleted.begin(), deleted.end(), 0u) == deleted.end());
    BOOST_CHECK(std::find(added.begin(), added.end(), 0u) == added.end());
}

// --- Correctness properties --------------------------------------------------

BOOST_AUTO_TEST_CASE(distance_is_symmetric)
{
    std::vector<int> a{1, 2, 3, 4}, b{1, 9, 3, 8};
    std::vector<size_t> d1, d2, a1, a2;
    auto dist1 = levenshtein_distance(make_equal(a, b), a.size(), b.size(), d1, a1);
    auto dist2 = levenshtein_distance(make_equal(b, a), b.size(), a.size(), d2, a2);
    BOOST_REQUIRE(dist1 && dist2);
    BOOST_CHECK_EQUAL(*dist1, *dist2);
}

BOOST_AUTO_TEST_CASE(distance_bounded_by_size_difference)
{
    std::vector<int> a{1, 2, 3, 4, 5}, b{1, 2};
    std::vector<size_t> deleted, added;
    auto d = levenshtein_distance(make_equal(a, b), a.size(), b.size(), deleted, added);
    BOOST_REQUIRE(d.has_value());
    BOOST_CHECK_GE(*d, 3u);  // at least |M - N| edits
}

// --- Large input (stress test) -----------------------------------------------

BOOST_AUTO_TEST_CASE(large_identical_sequences)
{
    const size_t N = 500;
    std::vector<int> a(N, 42), b(N, 42);
    std::vector<size_t> deleted, added;
    auto d = levenshtein_distance(make_equal(a, b), N, N, deleted, added);
    BOOST_REQUIRE(d.has_value());
    BOOST_CHECK_EQUAL(*d, 0u);
    BOOST_CHECK(deleted.empty());
    BOOST_CHECK(added.empty());
}

BOOST_AUTO_TEST_CASE(works_with_std_function)
{
    std::vector<int> a{1, 2, 3}, b{1, 4, 3};
    std::function<bool(size_t, size_t)> equal = [&](size_t i, size_t j) {
        return a[i] == b[j];
    };
    std::vector<size_t> deleted, added;
    auto d = levenshtein_distance(equal, a.size(), b.size(), deleted, added);
    BOOST_REQUIRE(d.has_value());
    BOOST_CHECK_EQUAL(*d, 2u);  // one substitution = delete + insert
}

BOOST_AUTO_TEST_SUITE_END()
