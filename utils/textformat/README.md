# Text document framing

Link `textformat_iface` and include `textformat/document.hpp`.

- `textformat::metadata(fields)` renders nonempty field text under `## Metadata`
  inside a Markdown code fence. Empty metadata produces no text.
- `textformat::literal_block(body)` renders literal text with a fence longer than
  any run of backticks in the body, with a minimum length of three. It adds a
  final newline if needed for the closing fence but does not escape the body.

These pure helpers have no tool, model, JSON, or IO dependency. Callers serialize
metadata fields and supply trusted labels. They provide visual boundaries, not
security boundaries. Intrinsic `ToolResult` uses them to keep metadata and hints
before each record's literal output.
