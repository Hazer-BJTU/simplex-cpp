#define BOOST_TEST_MODULE TextEdit
#include <boost/test/unit_test.hpp>

#include "fileio/replace_existing.hpp"
#include "textedit/edit.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;

struct Scratch {
    fs::path root;

    Scratch()
    {
        auto pattern = (fs::temp_directory_path() / "simplex-edit-XXXXXX").string();
        const auto directory = ::mkdtemp(pattern.data());
        if (!directory) throw std::runtime_error("cannot create edit test directory");
        root = directory;
    }

    ~Scratch()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    fs::path write(const std::string& contents) const
    {
        const auto path = root / "source";
        std::ofstream stream(path, std::ios::binary);
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        stream.close();
        BOOST_REQUIRE(stream.good());
        return path;
    }

    std::string read(const fs::path& path) const
    {
        std::ifstream stream(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(stream), {}};
    }
};
}

BOOST_AUTO_TEST_CASE(unique_edit_preserves_bytes_and_aligns_context)
{
    const auto plan = textedit::prepare_str_replace("one\r\nold\nend\n", "old", "new", 1);
    BOOST_CHECK(plan.result.status == textedit::EditStatus::Modified);
    BOOST_TEST(plan.updated_text == "one\r\nnew\nend\n");
    BOOST_TEST(plan.result.match_byte == 5u);
    BOOST_TEST(plan.result.before_position.line == 1u);
    BOOST_TEST(plan.result.before_position.column == 0u);
    BOOST_TEST(plan.result.before_excerpt == "  0 | one\n- 1 | old\n  2 | end");
    BOOST_TEST(plan.result.after_excerpt == "  0 | one\n+ 1 | new\n  2 | end");
    BOOST_TEST(!plan.result.preview_truncated);
}

BOOST_AUTO_TEST_CASE(overlaps_missing_and_identical_content_do_not_publish)
{
    const auto ambiguous = textedit::prepare_str_replace("aaa", "aa", "b");
    BOOST_CHECK(ambiguous.result.status == textedit::EditStatus::Ambiguous);
    BOOST_TEST(ambiguous.result.matches_at_least == 2u);
    BOOST_REQUIRE(ambiguous.result.second_match_byte.has_value());
    BOOST_TEST(*ambiguous.result.second_match_byte == 1u);
    BOOST_TEST(ambiguous.updated_text.empty());
    const auto missing = textedit::prepare_str_replace("aaa", "x", "b");
    BOOST_CHECK(missing.result.status == textedit::EditStatus::NotFound);
    BOOST_TEST(missing.result.matches_at_least == 0u);
    const auto same = textedit::prepare_str_replace("old", "old", "old", 0);
    BOOST_CHECK(same.result.status == textedit::EditStatus::Unchanged);
    BOOST_TEST(same.updated_text.empty());
    BOOST_TEST(same.result.before_excerpt == "= 0 | old");
    BOOST_TEST(same.result.after_excerpt == "= 0 | old");
    BOOST_CHECK_THROW((void)textedit::prepare_str_replace("x", "", "y"), std::invalid_argument);
    BOOST_CHECK(textedit::prepare_str_replace("x", "x", "y", 20).result.status ==
                textedit::EditStatus::Modified);
    BOOST_CHECK_THROW((void)textedit::prepare_str_replace("x", "x", "y", 21), std::invalid_argument);
    BOOST_CHECK_THROW((void)textedit::prepare_str_replace("x", "x", "too long", 3, 1),
                      std::length_error);
}

BOOST_AUTO_TEST_CASE(deletion_multiline_change_and_eof_anchor_are_marked)
{
    const auto removed = textedit::prepare_str_replace("a\nold\nb\n", "old\n", "", 1);
    BOOST_TEST(removed.updated_text == "a\nb\n");
    BOOST_TEST(removed.result.before_excerpt.find("- 1 | old") != std::string::npos);
    BOOST_TEST(removed.result.after_excerpt.find("+ 1 | b") != std::string::npos);
    const auto inserted = textedit::prepare_str_replace("a", "a", "a\nnew", 0);
    BOOST_TEST(inserted.result.after_excerpt == "+ 0 | a\n+ 1 | new");
    const auto end = textedit::prepare_str_replace("x\nold", "old", "", 0);
    BOOST_TEST(end.updated_text == "x\n");
    BOOST_TEST(end.result.after_excerpt == "+ 1 | ");
}

BOOST_AUTO_TEST_CASE(shared_number_width_and_long_span_omission)
{
    std::string source;
    for (int line = 0; line < 12; ++line) {
        source += line == 9 ? "needle\n" : "line\n";
    }
    const auto aligned = textedit::prepare_str_replace(source, "needle", "new", 2);
    BOOST_TEST(aligned.result.before_excerpt.find("-  9 | needle") != std::string::npos);
    BOOST_TEST(aligned.result.after_excerpt.find("+  9 | new") != std::string::npos);
    BOOST_TEST(aligned.result.before_excerpt.find("  10 | line") != std::string::npos);
    BOOST_TEST(aligned.result.after_excerpt.find("  10 | line") != std::string::npos);

    std::string many = "head\n";
    for (int line = 0; line < 100; ++line) many += "old\n";
    many += "tail\n";
    const auto long_edit = textedit::prepare_str_replace(
        many, many.substr(5, 400), "new\n", 3);
    BOOST_CHECK(long_edit.result.status == textedit::EditStatus::Modified);
    BOOST_TEST(long_edit.result.preview_truncated);
    BOOST_TEST(long_edit.result.before_excerpt.find("lines omitted") != std::string::npos);
    BOOST_TEST(long_edit.result.before_excerpt.find("-  ") != std::string::npos);
    BOOST_TEST(long_edit.result.before_excerpt.find("tail") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(long_and_invalid_lines_have_bounded_valid_display)
{
    const auto plan = textedit::prepare_str_replace(
        std::string(100000, '\x80') + "old", "old", "new", 0);
    BOOST_CHECK(plan.result.status == textedit::EditStatus::Modified);
    BOOST_TEST(plan.result.preview_truncated);
    BOOST_TEST(plan.result.before_excerpt.size() < 1024u);
    BOOST_TEST(plan.result.before_excerpt.find("\xef\xbf\xbd") != std::string::npos);
    BOOST_TEST(plan.result.before_excerpt.find("old") != std::string::npos);
    BOOST_TEST(plan.result.after_excerpt.find("new") != std::string::npos);
    BOOST_TEST(plan.result.before_excerpt.find("bytes skipped") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(file_edit_preserves_mode_and_declines_unsafe_targets, Scratch)
{
    const auto file = write("before\r\nold\n");
    BOOST_REQUIRE(::chmod(file.c_str(), 0640) == 0);
    const auto edited = textedit::str_replace_file(file, "old", "new", 1);
    BOOST_CHECK(edited.status == textedit::EditStatus::Modified);
    BOOST_TEST(read(file) == "before\r\nnew\n");
    BOOST_CHECK((fs::status(file).permissions() & fs::perms::mask) ==
                (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read));
    const auto unchanged = textedit::str_replace_file(file, "new", "new", 0);
    BOOST_CHECK(unchanged.status == textedit::EditStatus::Unchanged);
    const auto missing = textedit::str_replace_file(file, "absent", "x");
    BOOST_CHECK(missing.status == textedit::EditStatus::NotFound);
    BOOST_TEST(read(file) == "before\r\nnew\n");

    fs::create_symlink(file, root / "link");
    BOOST_CHECK_THROW((void)textedit::str_replace_file(root / "link", "new", "x"),
                      std::system_error);
    fs::create_hard_link(file, root / "hardlink");
    BOOST_CHECK_THROW((void)textedit::str_replace_file(file, "new", "x"),
                      std::system_error);
    BOOST_TEST(read(file) == "before\r\nnew\n");
}

BOOST_FIXTURE_TEST_CASE(fileio_detects_stale_expected_bytes_before_publish, Scratch)
{
    const auto file = write("old");
    auto original = fileio::read_editable_file(file, 1024);
    auto wrong_size = original;
    wrong_size.bytes = "older";
    BOOST_CHECK_THROW(fileio::replace_existing_file(file, wrong_size, "new"),
                      fileio::ReplaceConflict);
    auto wrong_bytes = original;
    wrong_bytes.bytes = "bad";
    BOOST_CHECK_THROW(fileio::replace_existing_file(file, wrong_bytes, "new"),
                      fileio::ReplaceConflict);
    BOOST_TEST(read(file) == "old");
    fileio::replace_existing_file(file, original, "new");
    BOOST_TEST(read(file) == "new");
    for (const auto& entry : fs::directory_iterator(root)) {
        BOOST_TEST(!entry.path().filename().string().starts_with(".simplex-edit-"));
    }
}

BOOST_FIXTURE_TEST_CASE(same_contents_on_a_replacement_inode_are_a_conflict, Scratch)
{
    const auto file = write("old");
    const auto initial = fileio::read_editable_file(file, 1024);
    const auto replacement = root / "replacement";
    {
        std::ofstream output(replacement, std::ios::binary);
        output << "old";
        BOOST_REQUIRE(output.good());
    }
    fs::rename(replacement, file);
    const auto current = fileio::read_editable_file(file, 1024);
    BOOST_TEST(current.bytes == initial.bytes);
    BOOST_TEST(current.identity.inode != initial.identity.inode);
    BOOST_CHECK_THROW(fileio::replace_existing_file(file, initial, "new"),
                      fileio::ReplaceConflict);
    BOOST_TEST(read(file) == "old");
}
