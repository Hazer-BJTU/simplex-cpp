# textedit

Text-file operations independent of model-facing toolsets. The package currently
provides advisory UTF-8 inspection, a byte-based line index, text reading, and
precise replacement. Editing and reading tools are separate consumers.

## Exact replacement

`textedit/edit.hpp` provides `prepare_str_replace` for a pure decision and
`str_replace_file` for publication to an existing file. `old_text` must be
nonempty and occur exactly once (overlapping matches count separately). An
empty `new_text` deletes it; an identical replacement reports `Unchanged`
without writing. The file path is not confined to a workspace. Files over
16 MiB, final symlinks, multiple hard links and special-mode files are
rejected. Existing owner, group and POSIX permissions are preserved. Other
inode metadata, such as ACLs and extended attributes, is not preserved.

Results include a before excerpt and an after excerpt with a shared line-number
width. `-` marks changed original lines, `+` marks changed replacement lines,
and `=` marks an unchanged match. `context_lines` defaults to 3 (range 0–20).
Each shown line is limited to 512 display bytes, with long changed lines
focused near the match; spans over 48 lines retain
their first and last 24 lines with an omission marker. These limits affect
only previews: the edit retains exact bytes, including newlines and invalid
UTF-8. `preview_truncated` reports clipping or omission.

Before publication, the file's bytes and identity are checked again.
`Conflict` means this call did not publish a replacement. Publication uses a
temporary file in the same directory, followed by rename and directory sync.
`PublishedSyncFailed` means new content is visible but crash durability is
uncertain; inspect the destination before retrying. Independent writers can
still race after the final check, so this is not a compare-and-swap protocol.

## Reading selections

Include `textedit/read.hpp`. `read_lines(text, start_line, line_count, format)`
and `read_bytes(text, start_byte, byte_count, format)` operate on borrowed string
views and return owned output. Their `read_file_lines` / `read_file_bytes`
counterparts accept a filesystem path. All offsets and line numbers are zero-based.

Line formats:

- `Plain`: exact selected bytes, with original newline sequences and no labels.
- `LineIndex`: decimal line number, right-aligned within the selection, then
  ` | ` and the line's content.
- `ByteRange`: `[start:end] | ` and content. Both decimal fields have the same
  width, chosen from the largest endpoint in the selection. The range is
  half-open and includes the original line terminator.

For example, reading `"abc\r\ndefgh\nZ"` as byte-range-indexed lines returns:

```text
[ 0: 5] | abc
[ 5:11] | defgh
[11:12] | Z
```

Indexed formats replace logical terminators with LF display separators, with
no additional final LF. Original offsets remain unchanged. Other content is
literal: tabs, NUL and terminal controls are not escaped or sanitized. An empty
logical line still has a label; in plain format it contributes zero bytes.
Line indexing follows `LineIndex`, including its trailing zero-byte line.

Byte formats are `Plain` (exact bytes) and `HexEscaped` (every byte becomes
`\xHH`, with uppercase hex digits and no separators). For example, the three
bytes `A`, NUL and LF become `\x41\x00\x0A`. Byte selection can split a UTF-8
character; neither mode enforces UTF-8 validity.

Results include `text`, full-input `total_lines` / `total_bytes`, original
`start_byte` / exclusive `end_byte`, and
`reached_end`. Line results also include `start_line` and `lines_read`.
`reached_end` indicates that there are no more entries in the selected mode:
after reading the bytes of a final newline, a zero-byte logical line can still
remain in line mode. Counts clamp to available entries without arithmetic
overflow. A zero count returns empty output at the requested position. Starting
exactly at the total line count or byte count returns an empty EOF selection;
starting beyond it throws `std::out_of_range`. Invalid format enums throw
`std::invalid_argument`.

Totals come from the same input bytes as the selection, including for empty
selections. Line totals follow `LineIndex` (empty input has one line, and a final
terminator introduces an empty line). Byte reads also scan the complete input
to compute total_lines; they do not count just the selected bytes.

File functions currently load the whole file before selection, bounded by an
optional `max_file_bytes` parameter (default 16 MiB). One lookahead byte detects
oversize input, which throws `std::length_error`; content is never silently
truncated to satisfy this bound. Zero allows an empty file, while `SIZE_MAX` is
invalid. The original unrestricted read functions can allocate a full line
index and full formatted selection; they are intended for callers that need
the complete result.

`read_lines_bounded` / `read_bytes_bounded` and their file counterparts enforce
an output byte budget during rendering. They count lines with constant index
space and never allocate a full line-start vector. Indexed labels and hex
escapes are produced only until the display budget is reached. Plain display
validates UTF-8 as it appends, replacing malformed bytes with U+FFFD without
splitting valid characters. `display_replaced` reports replacements in the
displayed prefix; `output_truncated` reports undisplayed selection content.
The file bytes remain subject to the input limit and are loaded once. These
bounded functions use O(file size + display budget) memory, excluding normal
allocator overhead and the small metadata/result objects.
For repeated selections, load once and use the string-view functions. Reads are
synchronous and use `fileio`'s regular-file checks and symlink handling. Filesystem
errors throw `std::system_error`; concurrent modifications are not a snapshot.
No files are modified, and UTF-8 probing remains a separate advisory operation.

## Line and byte-column indexing

Include `textedit/line_index.hpp` and construct `LineIndex` from a string view:

```cpp
const textedit::LineIndex index("ab\r\nX");
const auto position = index.position_at(3); // {line: 0, column: 3}: LF byte
const auto offset = index.offset_at({1, 0}); // 4: X byte
const auto length = index.line_byte_count(0); // 4, including CR and LF
const auto eof = index.position_at(5); // {line: 1, column: 1}
```

Both coordinates start at zero. Columns count bytes, including individual bytes
inside a UTF-8 character, BOM, tab or newline; they are not display columns.
Recognized terminators are LF, CRLF, CR, VT, FF, and UTF-8 NEL (U+0085), LS
(U+2028), PS (U+2029). CRLF is one terminator. Every terminator byte belongs to
the line it ends, and the next line starts immediately after the terminator.
Other bytes are unchanged; malformed UTF-8 is accepted and raw byte 0x85 is
ordinary content. UTF-8 newline byte patterns are recognized without validating
the surrounding text.

Empty input has one zero-byte line. A trailing terminator adds a zero-byte final
line: `"a\n"` has line lengths `[2, 0]`. Offset `byte_count()` represents EOF,
at the final line's end column. Only that line accepts a column equal to its byte
count; earlier lines' end columns are invalid rather than aliases of the next
line's start. This gives a unique bidirectional mapping for every byte and EOF.
Invalid offsets, lines and columns throw `std::out_of_range`.

Construction scans once in O(bytes) time and stores only line starts and total
length in O(lines) memory. `position_at()` uses binary search, O(log lines).
`offset_at()`, `line_start()` and `line_byte_count()` are O(1). No text view is
retained: the source may be destroyed, but an index must be rebuilt before use
with modified contents. Concurrent const lookups are supported while the index
is alive and not assigned to or moved from.

## Advisory UTF-8 inspection

Link `textedit_lib` and include `textedit/utf8_probe.hpp`:

```cpp
const auto result = textedit::probe_utf8_file(path);
if (result.likelihood == textedit::Utf8TextLikelihood::Unlikely) {
    // Attach a hint: "This file may not be UTF-8 text."
}
if (result.error) {
    // Report an inspection failure, not an encoding warning.
}
```

The default sample is the first 8192 bytes, with one extra byte to determine
whether the file continues. Callers can select 1 through 1048576 sample bytes.
Memory and inspected content are bounded by this limit. IO is synchronous.
Regular files are accepted, including symlink targets; other file types are
refused. Relative paths use the process working directory. The probe never
writes file contents, but normal filesystem access-time updates can occur.

`Likely` means the sampled prefix is compatible with UTF-8 text, not that the
file is proven to be text or entirely UTF-8. ASCII, empty files and a UTF-8 BOM
are accepted. `Unlikely` means malformed UTF-8 or suspicious ASCII controls
were found. `Unknown` with an error code means inspection failed. None of these
results prohibits a caller from reading or editing the file.

Validation rejects stray continuation bytes, overlong encodings, surrogates,
values above U+10FFFF and incomplete sequences at EOF. A potentially valid
sequence cut off by the sample limit sets `incomplete_suffix` instead of an
encoding error. Already-invalid partial sequences are still rejected. The
first invalid sequence's byte offset is reported when present. NUL, DEL and
ASCII controls other than TAB, LF, CR and FF independently produce a warning;
legitimate text containing those characters can therefore receive a warning.

This is deliberately a heuristic, not encoding detection. ASCII-only binary
data and some legacy-encoded data can pass. Invalid bytes after the sample are
not examined. No conversion, normalization or full-file scan occurs. Files
changing during inspection are not a consistent snapshot, and this result must
not serve as a write precondition or security decision. Allocation failures can
throw; invalid sample limits throw `std::invalid_argument`.
