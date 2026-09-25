# Plain metadata formatting

Link `textformat_iface` and include `textformat/document.hpp`.

`textformat::metadata_field(name, value)` produces one `[[name]]: value` line,
including a final newline. It adds no Markdown heading or code fence.

Callers supply trusted labels and serialize values to one line, escaping control
characters where needed. Empty-field policy belongs to the caller. The helper
has no tool, model, JSON, or IO dependency. Intrinsic `ToolResult` uses it to put
metadata and hints before literal output. Markers provide visual distinction;
output may contain similar text, so they are not a security boundary.
