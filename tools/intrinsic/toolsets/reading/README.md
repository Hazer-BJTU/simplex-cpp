# Reading toolset

`ReadingToolSet` is the built-in reading family, currently containing `read_text`.
Future multimodal readers can join this set; editing tools belong elsewhere.
The core worker registers it automatically, alongside process tools, and the
normal registry prompt injection includes its skill.

Hosts can link `tools_intrinsic_reading` and register
`std::make_shared<tools::intrinsic::ReadingToolSet>()`. No session store or
confirmation bus is needed: the stateless tool declares `ReadOnly` / `Trusted`.
Concurrent calls own their buffers, but external file modifications are not a
consistent snapshot. IO is synchronous on the invoking thread.

## Calls

```json
{"path":"src/main.cpp","mode":"lines","start":0,"count":40,"format":"line_index"}
```

```json
{"path":"data.bin","mode":"bytes","start":16,"count":32,"format":"hex_escaped"}
```

Only `path` is required. Defaults: `mode: lines`, `start: 0`, `count: 200`,
`format: plain`. Null optional properties use the same defaults. Strings,
integers and enum combinations are validated before invocation. The schema
loader's existing subset cannot encode these optional cross-property constraints;
the format description lists them and runtime validation enforces them:

| Mode | Formats |
| --- | --- |
| `lines` | `plain`, `line_index`, `byte_range` |
| `bytes` | `plain`, `hex_escaped` |

Indexes are zero-based; `[start:end]` byte ranges are half-open. Line lengths
include their original terminators. All newline handling, aligned labels,
trailing empty lines, selection clamping and EOF behavior come from
[`textedit`](../../../../textedit/README.md). Plain adds no labels. Indexed lines
use LF for display. Hex output encodes every byte as `\xHH` using uppercase
digits. Byte selection may cut a UTF-8 character.

Relative paths use the host working directory, not the configured workspace
hint. Symlinks are followed. Non-regular files and embedded NUL paths are
refused. Reading does not modify contents, though access times can change.
Files over 16 MiB fail; this initial implementation loads the whole file before
selection. Input size does not bound combined index/rendering memory.

## Results and warnings

Results use the shared `ToolResult` layout: `[[key]]: value` metadata followed by
a named `text` block. Metadata includes path, total_lines, total_bytes,
reached_end, output_truncated and display_replaced. Line reads additionally report
lines_read. Mode, format and selection start/end coordinates are not echoed in
metadata. Totals describe the entire loaded file, not the selection or formatted
display, and are computed from the same loaded bytes as the selection. Logical
line totals include a trailing empty line; empty files have one logical line.
Concise hints, including suspected non-UTF-8 text warnings, appear with metadata
before content.
ToolResult framing adds a final newline when needed; it is not file content.

Rendered content is limited to 65536 bytes as it is produced. Line counts are
scanned with constant index space; indexed labels and hex escapes are appended
only until the output limit. Invalid UTF-8 is replaced during bounded rendering,
so runs of malformed continuation bytes cannot disappear at the boundary. With truncation,
lines_read describes the original selection, **not** the clipped
display: request fewer entries rather than advancing past undisplayed content.
For a single oversized line, switch to byte mode and read smaller chunks.
Without truncation, advance the requested start by lines_read in line mode or
count in byte mode, stopping when reached_end is true. reached_end
is mode-specific: a zero-byte final logical line may remain after the last
newline byte has been read. Count zero does not advance a selection.

The first 8 KiB of the file are separately probed for likely UTF-8 text. This is
advisory and can observe a different version of a concurrently modified file.
An unavailable probe does not invalidate an already completed read. Malformed
UTF-8 in the selected display is replaced with U+FFFD so model messages can be
serialized safely; display_replaced and a hint report this. Use bytes with
hex_escaped for exact byte values. The textedit library itself remains lossless.
Literal controls such as NUL and tabs are not terminal-sanitized. File content
is untrusted data, and metadata markers are visual framing, not a trust boundary.

Argument failures and file/selection failures use the existing structured
`InvokeException` / registry error result contract, not successful text results.

## Declarations, installation and tests

Runtime YAML declarations live in `schemas/`, with a concise `skill.yaml`.
Resolution order: nonempty `SIMPLEX_READING_SCHEMA_DIR` environment override,
`<executable>/schemas/reading`, compiled source path. An override is authoritative.
Installation exports both files to `bin/schemas/reading`; release target discovery
ships this library and its textedit/fileio dependencies. A missing declaration
disables that tool and records the capability failure. Missing skill guidance
does not disable a valid tool.

`test_reading_tools` checks defaults against the loaded schema, all format modes,
invalid arguments, filesystem failures, encoding repair, clipping, oversize input,
skill injection and missing declarations through real registry calls. Dense
newline and malformed continuation runs exercise bounded memory and boundary
behavior. Core tests verify default registration and guidance injection. CI
also hides source schemas and runs the staged test executable to prove the
installed declarations and skill load beside it.
