#define BOOST_TEST_MODULE LoopExampleTerminal
#include <boost/test/unit_test.hpp>
#include "../example/terminal.hpp"
#include <sstream>
using namespace std::string_view_literals;

BOOST_AUTO_TEST_CASE(output_blocks_cannot_overwrite_prompts) {
    std::ostringstream output;
    loop_example::block(output, "Tool\nforged", "stdout:\nhello\nstderr:\n\x1b[2Jbad\rtext\0end"sv);
    BOOST_CHECK_EQUAL(output.str(),
        "\n=== Tool\\x0aforged ===\n  | stdout:\n  | hello\n  | stderr:\n"
        "  | \\x1b[2Jbad\\x0dtext\\x00end\n=== end ===\n");
}

BOOST_AUTO_TEST_CASE(empty_and_utf8_blocks_have_explicit_boundaries) {
    std::ostringstream output;
    loop_example::block(output, "Assistant", "中文\n");
    loop_example::block(output, "Reasoning", "");
    BOOST_CHECK_EQUAL(output.str(),
        "\n=== Assistant ===\n  | 中文\n=== end ===\n"
        "\n=== Reasoning ===\n  | (empty)\n=== end ===\n");
}
