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
so inspect the file before another edit. Existing regular files up to 16 MiB
are supported. Final symlinks, multiple hard links and special-mode files are
refused. Owner, group and POSIX mode are preserved, while ACLs and extended
attributes are not. The tool does not restrict paths to the configured
workspace.
