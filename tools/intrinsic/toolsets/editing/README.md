# Intrinsic editing tools

The `editing` toolset currently provides `str_replace_edit`. The worker
registers it by default. Its declarations and model-facing skill live in
`schemas/` and are installed beside the worker at `bin/schemas/editing/`.

Pass an existing file path, a nonempty `old_text` copied exactly from that
file, and `new_text`. The original span must occur once; include surrounding
text when a short span is ambiguous. Whitespace and line endings matter.
Empty `new_text` deletes the span. The tool requires confirmation before it
reads or writes the file. It is a serial write operation.

The result uses `[[...]]` metadata followed by separate `before` and `after`
blocks. Both blocks use the same line-number width, `-` and `+` mark affected
lines, and `context_lines` selects 0–20 unchanged lines on each side (default
3). A long line or edit shows an explicit clipping or omission marker.
`preview_truncated` reports this condition. The preview is for review; it does
not modify the bytes used for the actual replacement.

A missing or ambiguous match and a concurrent file conflict are tool errors;
the destination is not changed by that call. `published_sync_failed` is
different: the new bytes are visible, but directory durability is uncertain,
so inspect the file before another edit. Existing regular files up to the
configured host limit (default 1 GiB, also the largest configurable value) are
supported. Final symlinks, multiple hard links and special-mode files are
refused. Owner, group and POSIX mode are preserved, while ACLs and extended
attributes are not. The tool does not restrict paths to the configured
workspace.

The host scans the source once and stages the replacement in a private candidate
beside the destination, so neither the source nor the result is buffered in
memory; a temporary file roughly the size of the edited output exists until
publication. Before publishing, the candidate is compared against the unchanged
source: the shared prefix, the matched span byte for byte, then the suffix
through EOF. Exact comparison is never replaced by a digest or timestamps.

## Host configuration

`schemas/str_replace_edit.yaml` carries an optional construction-time limit
beside the model-facing declaration:

```yaml
config:
  max_file_bytes: 1073741824
```

`max_file_bytes` accepts 1–1073741824 and bounds both the scanned source and the
resulting size. Omitted values keep the 1 GiB default. Null, booleans, floats,
strings, unknown keys and out-of-range values disable this tool with file/field
diagnostics. The setting belongs to this tool instance: it never appears in
advertised schemas or invocation arguments, so a model cannot widen or narrow
it. A selected `SIMPLEX_EDITING_SCHEMA_DIR` override replaces the installed
declaration as a whole.
