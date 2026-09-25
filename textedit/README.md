# textedit

Text-file operations independent of model-facing toolsets. The package currently
provides advisory UTF-8 inspection and a byte-based line index; editing and
reading tools will be separate consumers of the package.

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
