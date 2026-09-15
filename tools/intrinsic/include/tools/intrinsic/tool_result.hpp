#pragma once

//
// tool_result.hpp — how an intrinsic tool answers a call
// ======================================================
//
// A tool's result is read twice: by the model, which acts on it, and by a human
// reading the transcript over its shoulder. Both want the same thing — the few
// facts about the call, and whatever the tool produced — which is why the result
// is a small document rather than a serialisation of one:
//
//   session_id: proc_1
//   state: exited
//   exit_code: 0
//   executable: seq
//   arguments: ["1", "5"]
//   output_complete: true
//   running_milliseconds: 12
//
//   stdout (10 bytes):
//   1
//   2
//   3
//   4
//   5
//
// WHY NOT JSON, which is what this replaces. An object has to carry every string
// INSIDE a string, so the one part a caller actually asked for — what the child
// printed — arrives escaped: `"stdout_text": "1\n2\n3\n"`. The bytes are then
// not the bytes any more, a reader has to un-escape them to see the output, and
// everything else is buried between braces and quoted keys. Field-per-line text
// carries the same facts with no encoding in the way, and the raw text is raw.
//
// WHAT THIS IS NOT: a machine format. Nothing in this tree parses a result — the
// model reads it, and a host that wants the facts programmatically has the
// store, the snapshot and the record's own fields. So the format is chosen for
// the reader first, and a field whose value is prose stays prose.
//
// THE RULES, in full, because the shape is a contract the tests assert on:
//
//   field(name, value)  one line, `name: value`, in the order added.
//                       A string is written VERBATIM when it is one line, so a
//                       path, a command and a label read as themselves. A value
//                       that needs more than one line — or is not a string —
//                       is written as compact JSON, which is the only way one
//                       line can hold it. A field with nothing in it (an empty
//                       string, an empty array, null) writes NOTHING: `label:`
//                       with no value tells a reader less than no line at all.
//   block(name, text)   the text, verbatim, under a header line that names it
//                       and says how much of it there is —
//                       `name (10 bytes):`. Nothing is escaped, indented or
//                       folded: what the caller produced is what the reader
//                       sees. Truncated text says so — `name (truncated, first
//                       4096 bytes):` — and an empty block is `name: (empty)`,
//                       because "it printed nothing" is an answer.
//   separate()          a `---` line, so a result about several things (one
//                       record per session, say) reads as several records with
//                       a visible edge between them rather than as one long
//                       run. Idempotent, and never first.
//
// A block is always preceded and followed by a blank line, which is what keeps
// the output visually apart from the metadata around it; a value that is empty
// simply leaves its line out. A result about one thing therefore reads as
// field lines with, under them, the text they are about — and a blank line is
// never what tells two records apart, because a block is surrounded by them
// too.
//
// ONE THING A READER SHOULD KNOW: the text of a block is whatever the tool
// produced, so a line inside it may look like a field or like another block's
// header. That ambiguity is the price of not escaping the bytes, it only matters
// to a parser (there is none), and a reader has the header above the text to
// tell them where it started.
//

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "dataclass/model_io.hpp"

namespace tools::intrinsic {

/**
 * One tool result, under construction: the fields a reader scans and the
 * verbatim blocks they read, in the order they are added, rendered into the
 * single text part a call answers with.
 *
 * See the file header for the format and for why it is this and not JSON. The
 * builder holds the rendered text as it goes, so render() is a copy and the
 * object is cheap to use from a coroutine.
 */
class ToolResult {
public:
    /**
     * One `name: value` line, or nothing at all when the value says nothing
     * (empty string, empty array, null).
     *
     * @param name the field's name, as a reader knows it (`session_id`,
     *        `exit_code`). Written verbatim: it is this tree's own vocabulary,
     *        not anything a caller sent.
     * @param value the value. Strings are written as themselves when they are
     *        one line and as compact JSON when they are not; everything else —
     *        numbers, booleans, arrays — is compact JSON.
     */
    ToolResult& field(std::string_view name, nlohmann::json value);

    /**
     * A block of text, carried VERBATIM — this is the call for a child's
     * output, a file's contents, anything whose bytes are the point. The
     * header line names it and counts it; nothing about the text itself is
     * touched.
     *
     * @param name what the text is (`stdout`, `new_stdout`).
     * @param text the text. Empty is legal and renders as `name: (empty)`.
     * @param truncated whether what is here is only the beginning of what the
     *        tool has; the header says so, because otherwise a reader would
     *        take a cut-off capture for the whole of it.
     */
    ToolResult& block(std::string_view name, std::string text,
                      bool truncated = false);

    /// A `---` line, so what follows reads as a separate record: a poll
    /// answers about several sessions at once, and each one should start at a
    /// line a reader can find. Nothing happens for a result that has not
    /// started yet, and two calls in a row write one rule.
    ToolResult& separate();

    /// The result as the model reads it: a text part, and nothing else.
    [[nodiscard]] model_io::Content render() const;

    /// The rendered text alone, which is what render() puts in the part. Always
    /// the exact bytes render() answers with, so a test may hold either.
    [[nodiscard]] const std::string& text() const noexcept { return _text; }

    /// Whether anything has been written yet. A result with no fields and no
    /// blocks renders as an empty text part.
    [[nodiscard]] bool empty() const noexcept { return _text.empty(); }

private:
    /// Close the current line, if it is not closed.
    void end_line();
    /// Mark that a blank line belongs before whatever comes next.
    void blank_line();
    /// Begin an element: close the line, and write the pending blank line.
    void start_element();

    std::string _text;
    bool _pending_blank = false;
    bool _last_was_rule = false;
};

} // namespace tools::intrinsic
