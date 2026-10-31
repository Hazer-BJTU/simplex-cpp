"""Bounded UTF-8 reading and explicit, optimistic file editing on POSIX.

Offsets always refer to the opened version. No implicit saves, undo stack,
cross-invocation handles, or whole-file buffers. Publication uses the pinned
parent directory and a same-directory temporary, not a path-based reopen.
"""

import codecs
import functools
import os
import re
import stat
import uuid
from collections import deque
from dataclasses import asdict, dataclass

from pydantic_monty import ClassInstance

from ..resources import MAX_OPEN_FILES, MAX_STAGED_BYTES, abort_file_operations, current_resources

BLOCK_BYTES = 64 * 1024
MAX_FILE_BYTES = 1024 * 1024 * 1024
MAX_OUTPUT_BYTES = 64 * 1024
MAX_MATCH_BYTES = 64 * 1024
MAX_EDITS = 128


@dataclass(frozen=True)
class TextSlice:
    """An exact, bounded UTF-8 prefix of a selected original-file range."""

    text: str
    start_byte: int
    end_byte: int
    start_line: int | None
    end_line: int | None
    truncated: bool

    def to_dict(self) -> dict:
        """Detach all fragment data for persistence or final output."""
        return asdict(self)

    def render(self) -> str:
        """Return the selected text without inventing lines or modifying newlines."""
        return self.text


@dataclass(frozen=True)
class EditPreview:
    """Bounded before/after display with aligned zero-based line numbers."""

    text: str
    edit_count: int
    truncated: bool

    def to_dict(self) -> dict:
        """Return a plain preview snapshot, never an editable handle."""
        return asdict(self)

    def render(self) -> str:
        """Display '-' old lines, '+' new lines and unmarked context."""
        return self.text


@dataclass(frozen=True)
class CommitResult:
    """A completed publication or a no-op, not a promise of future file identity."""

    path: str
    changed: bool
    edit_count: int
    original_size_bytes: int
    size_bytes: int

    def to_dict(self) -> dict:
        """Detach the receipt; subsequent Python failure cannot undo this commit."""
        return asdict(self)

    def render(self) -> str:
        """Summarize publication without embedding the entire file."""
        return f"{'committed' if self.changed else 'unchanged'}: {self.path}; edits={self.edit_count}; bytes={self.size_bytes}"


def _wrap(value):
    """Expose only flat result fields and explicit pure-data/display methods."""
    return ClassInstance(
        value, eager_attrs=tuple(value.__dataclass_fields__), allowed_methods={"to_dict", "render"}
    )


def _integer(value, name):
    if type(value) is not int or value < 0:
        raise ValueError(f"{name} must be a nonnegative integer")


def _signature(info):
    return (
        info.st_dev,
        info.st_ino,
        info.st_size,
        info.st_mtime_ns,
        info.st_ctime_ns,
        info.st_mode,
        info.st_uid,
        info.st_gid,
        info.st_nlink,
    )


def _file_method(method):
    """Invalidate all editors on method binding or execution errors."""

    @functools.wraps(method)
    def guarded(self, *args, **kwargs):
        try:
            return method(self, *args, **kwargs)
        except BaseException as exc:
            try:
                self._scope.abort()
            except BaseException as cleanup:
                exc.add_note(f"file cleanup also failed: {cleanup}")
            raise

    return guarded


def _operation(method):
    """Validate original identity around reads/edits and poison work on any error."""

    @_file_method
    @functools.wraps(method)
    def guarded(self, *args, **kwargs):
        self._check()
        result = method(self, *args, **kwargs)
        if self._fd is not None:
            self._check()
        return result

    return guarded


def _line_fragments(chunks):
    """Yield bounded (bytes, ends_line) fragments for LF, CRLF and CR.

    A trailing CR is held for one chunk to distinguish CRLF. No full line is
    accumulated, even when a file consists of a single enormous line.
    """
    pending = b""
    open_line = False
    for chunk in chunks:
        data = pending + chunk
        pending = b""
        if data.endswith(b"\r"):
            pending, data = b"\r", data[:-1]
        start = 0
        for match in re.finditer(b"\r\n|\r|\n", data):
            yield data[start : match.end()], True
            start = match.end()
            open_line = False
        if start < len(data):
            yield data[start:], False
            open_line = True
    if pending:
        yield pending, True
    elif open_line:
        yield b"", True


def _decode_prefix(raw: bytes, truncated: bool):
    """Decode strictly; only a budget-cut trailing code point may be withheld."""
    decoder = codecs.getincrementaldecoder("utf-8")()
    text = decoder.decode(raw, final=not truncated)
    retained = len(raw) - len(decoder.getstate()[0])
    return text, retained


def _preview_rows(chunks, ranges, context, budget=24 * 1024):
    """Stream lines into bounded context windows; long lines retain a prefix only."""
    ring = deque(maxlen=context)
    rows = []
    used = 0
    pos = start = line = 0
    last = -1
    after = 0
    prefix = bytearray()
    clipped = False
    truncated = False
    maximum = max(end for _, end in ranges)

    def append(row, mark):
        nonlocal used, last, truncated
        number, text, was_clipped = row
        if number <= last:
            return True
        cost = len(text.encode()) + 40
        if used + cost > budget:
            return False
        truncated |= was_clipped
        rows.append((number, mark, text))
        used += cost
        last = number
        return True

    for piece, ends in _line_fragments(chunks):
        kept = min(len(piece), 4096 - len(prefix))
        prefix.extend(piece[:kept])
        clipped |= kept < len(piece)
        pos += len(piece)
        if not ends:
            continue
        text, _ = _decode_prefix(bytes(prefix), clipped)
        text = text.rstrip("\r\n") + (" … [line truncated]" if clipped else "")
        hit = any((a < pos and b > start) if a != b else start <= a < pos for a, b in ranges)
        row = (line, text, clipped)
        if hit:
            for previous in ring:
                if not append(previous, False):
                    return rows, True
            if not append(row, True):
                return rows, True
            after = context
        elif after:
            if not append(row, False):
                return rows, True
            after -= 1
        truncated |= clipped and (hit or after > 0)
        ring.append(row)
        line += 1
        start = pos
        prefix.clear()
        clipped = False
        if pos > maximum and after == 0:
            break
    # An insertion/deletion at EOF needs an explicit anchor even in an empty file.
    if any(a == b == pos for a, b in ranges):
        for previous in ring:
            if not append(previous, False):
                return rows, True
        if not append((line, "[EOF]", False), True):
            return rows, True
    return rows, truncated


class TextFile:
    """Invocation-local file editor. Failure invalidates all outstanding editors.

    Public properties describe the opened version, never a moving filesystem
    view. All changes refer to original byte offsets; close/discard never save.
    """

    def __init__(self, path: str, writable: bool, scope):
        # Anchor relative lookup without lexical normalization: alias/../file
        # must let the kernel follow alias before applying '..'. The same
        # spelling is retained for conflict checks and the display path.
        self.path = path if os.path.isabs(path) else os.path.join(os.getcwd(), path)
        self._scope = scope
        self._writable = writable
        self._fd = self._parent_fd = None
        self._edits = []
        self._staged = 0
        self._parent, self._name = os.path.split(self.path)
        try:
            self._parent_fd = os.open(self._parent, os.O_RDONLY | os.O_DIRECTORY)
            candidate = os.stat(self._name, dir_fd=self._parent_fd, follow_symlinks=False)
            if not stat.S_ISREG(candidate.st_mode):
                raise ValueError(
                    "open_text requires an existing regular file, not a symlink/device/FIFO"
                )
            self._fd = os.open(
                self._name,
                (os.O_RDWR if writable else os.O_RDONLY) | os.O_NOFOLLOW | os.O_NONBLOCK,
                dir_fd=self._parent_fd,
            )
            self._info = os.fstat(self._fd)
            if not stat.S_ISREG(self._info.st_mode):
                raise ValueError(
                    "open_text requires an existing regular file, not a symlink/device/FIFO"
                )
            if self._info.st_size > MAX_FILE_BYTES:
                raise ValueError("file exceeds 1 GiB limit")
            if writable and (self._info.st_nlink != 1 or self._info.st_mode & 0o7000):
                raise ValueError(
                    "editing requires a single-link file without special permission bits"
                )
            if writable and hasattr(os, "listxattr") and os.listxattr(self._fd):
                raise ValueError("editing files with extended attributes/ACLs is not supported")
            self.size_bytes = self._info.st_size
            self._version = _signature(self._info)
            self._check()
            scope.files.add(self)
        except BaseException:
            self._release()
            raise

    def _check(self):
        if self._fd is None:
            raise RuntimeError("file editor is closed or invalidated; open it again")
        try:
            parent = os.stat(self._parent)
            pinned = os.fstat(self._parent_fd)
            current = os.stat(self._name, dir_fd=self._parent_fd, follow_symlinks=False)
        except (FileNotFoundError, NotADirectoryError) as exc:
            raise RuntimeError(
                "file changed during the edit operation; open it again before retrying"
            ) from exc
        if (
            (parent.st_dev, parent.st_ino) != (pinned.st_dev, pinned.st_ino)
            or _signature(current) != self._version
            or _signature(os.fstat(self._fd)) != self._version
        ):
            raise RuntimeError(
                "file changed during the edit operation; open it again before retrying"
            )

    def _range(self, start=0, end=None):
        end = self.size_bytes if end is None else end
        while start < end:
            chunk = os.pread(self._fd, min(BLOCK_BYTES, end - start), start)
            if not chunk:
                raise RuntimeError("file changed during reading")
            start += len(chunk)
            yield chunk

    def _modified(self):
        """One complete source scan, including removed bytes, with UTF-8 validation."""
        decoder = codecs.getincrementaldecoder("utf-8")()
        cursor = 0
        for start, end, replacement in self._edits:
            for chunk in self._range(cursor, start):
                decoder.decode(chunk)
                yield chunk
            for chunk in self._range(start, end):
                decoder.decode(chunk)
            if replacement:
                yield replacement
            cursor = end
        for chunk in self._range(cursor):
            decoder.decode(chunk)
            yield chunk
        decoder.decode(b"", final=True)

    @_operation
    def read_bytes(self, start: int, count: int) -> ClassInstance:
        """Read an exact UTF-8 range, trimming only an output-budget-cut suffix."""
        _integer(start, "start")
        _integer(count, "count")
        start = min(start, self.size_bytes)
        length = min(count, self.size_bytes - start)
        truncated = length > MAX_OUTPUT_BYTES
        raw = b"".join(self._range(start, start + min(length, MAX_OUTPUT_BYTES)))
        text, kept = _decode_prefix(raw, truncated)
        return _wrap(TextSlice(text, start, start + kept, None, None, truncated))

    @_operation
    def read_lines(self, start: int = 0, count: int = 200) -> ClassInstance:
        """Select logical lines without allocating the skipped prefix or huge lines."""
        _integer(start, "start")
        _integer(count, "count")
        data = bytearray()
        line = position = 0
        begin = None
        end_line = None
        truncated = False
        for piece, ends in _line_fragments(self._range()):
            if line >= start:
                if begin is None:
                    begin = position
                if line >= start + count:
                    break
                available = MAX_OUTPUT_BYTES - len(data)
                kept_piece = piece[:available]
                data.extend(kept_piece)
                if kept_piece:
                    end_line = line + 1
                if len(piece) > available:
                    truncated = True
                    break
            position += len(piece)
            if ends:
                line += 1
        if begin is None:
            begin = self.size_bytes
        text, kept = _decode_prefix(bytes(data), truncated)
        actual_start = min(start, line)
        return _wrap(
            TextSlice(
                text,
                begin,
                begin + kept,
                actual_start,
                end_line if end_line is not None else actual_start,
                truncated,
            )
        )

    def _add_edit(self, start, end, text):
        if not self._writable:
            raise RuntimeError("editor is read-only; open with writable=True")
        _integer(start, "start_byte")
        _integer(end, "end_byte")
        if end < start or end > self.size_bytes:
            raise ValueError("edit range must satisfy 0 <= start <= end <= original size")
        if not isinstance(text, str):
            raise TypeError("replacement text must be a string")
        if len(text) > MAX_STAGED_BYTES:
            raise ValueError("replacement exceeds staged byte budget")
        replacement = text.encode("utf-8")
        if len(self._edits) >= MAX_EDITS:
            raise ValueError("too many staged edits")
        for offset in (start, end):
            byte = os.pread(self._fd, 1, offset)
            if byte and byte[0] & 0xC0 == 0x80:
                raise ValueError("edit offset splits a UTF-8 character")
        for a, b, _ in self._edits:
            overlaps = start < b and a < end
            if start == end:
                overlaps = a <= start <= b
            elif a == b:
                overlaps = start <= a <= end
            if overlaps:
                raise ValueError(
                    "edits overlap or share an insertion boundary; all staged changes discarded"
                )
        if len(replacement) == end - start and replacement == b"".join(self._range(start, end)):
            return
        self._scope.reserve(len(replacement))
        self._staged += len(replacement)
        self._edits.append((start, end, replacement))
        self._edits.sort(key=lambda item: item[0])
        size = self.size_bytes + sum(len(data) - (b - a) for a, b, data in self._edits)
        if size > MAX_FILE_BYTES:
            raise ValueError("resulting file exceeds 1 GiB limit")

    @_operation
    def replace_range(self, start_byte: int, end_byte: int, text: str) -> None:
        """Stage a replacement against original [start_byte, end_byte) offsets."""
        self._add_edit(start_byte, end_byte, text)

    @_operation
    def insert(self, byte_offset: int, text: str) -> None:
        """Stage an insertion at an original UTF-8 boundary, including EOF."""
        self._add_edit(byte_offset, byte_offset, text)

    @_operation
    def replace(self, old_text: str, new_text: str) -> None:
        """Find exactly one byte match with bounded overlap buffers; stage, do not save."""
        if not self._writable:
            raise RuntimeError("editor is read-only; open with writable=True")
        if not isinstance(old_text, str) or not old_text:
            raise ValueError("old_text must be nonempty text")
        if not isinstance(new_text, str):
            raise TypeError("new_text must be text")
        if len(old_text) > MAX_MATCH_BYTES:
            raise ValueError("old_text exceeds 64 KiB match limit")
        pattern = old_text.encode("utf-8")
        if len(pattern) > MAX_MATCH_BYTES:
            raise ValueError("old_text exceeds 64 KiB match limit")
        previous = b""
        position = 0
        match = None
        for chunk in self._range():
            data = previous + chunk
            base = position - len(previous)
            offset = 0
            while True:
                found = data.find(pattern, offset)
                if found < 0:
                    break
                absolute = base + found
                if match is None:
                    match = absolute
                elif absolute != match:
                    raise ValueError(
                        "old_text must match exactly once; all staged changes discarded"
                    )
                offset = found + 1
            position += len(chunk)
            previous = data[-(len(pattern) - 1) :] if len(pattern) > 1 else b""
        if match is None:
            raise ValueError("old_text was not found; all staged changes discarded")
        self._add_edit(match, match + len(pattern), new_text)

    @_operation
    def preview(self, context_lines: int = 3) -> ClassInstance:
        """Stream before/after windows; never materialize the entire edited file."""
        _integer(context_lines, "context_lines")
        if context_lines > 20:
            raise ValueError("context_lines must be between 0 and 20")
        if not self._edits:
            return _wrap(EditPreview("No staged changes.", 0, False))
        before_ranges = [(a, b) for a, b, _ in self._edits]
        after_ranges = []
        delta = 0
        for start, end, text in self._edits:
            after_ranges.append((start + delta, start + delta + len(text)))
            delta += len(text) - (end - start)
        before, cut_before = _preview_rows(self._range(), before_ranges, context_lines)
        after, cut_after = _preview_rows(self._modified(), after_ranges, context_lines)
        width = max((len(str(row[0])) for row in before + after), default=1)
        lines = []
        for title, rows, marker in (("Before", before, "-"), ("After", after, "+")):
            lines.append(title)
            last = None
            for number, changed, text in rows:
                if last is not None and number != last + 1:
                    lines.append("  …")
                lines.append(f"{marker if changed else ' '} {number:>{width}} | {text}")
                last = number
        truncated = cut_before or cut_after
        if truncated:
            lines.append("[Preview truncated; staged edits are unchanged.]")
        return _wrap(EditPreview("\n".join(lines), len(self._edits), truncated))

    @_operation
    def commit(self) -> ClassInstance:
        """Publish one file atomically, with explicit uncertainty after rename starts."""
        if not self._writable:
            raise RuntimeError("editor is read-only; open with writable=True")
        count = len(self._edits)
        if not count:
            result = CommitResult(self.path, False, 0, self.size_bytes, self.size_bytes)
            self._release()
            return _wrap(result)
        temporary = f".simplex-edit-{uuid.uuid4().hex}"
        phase = "preparing"
        created = False
        try:
            fd = os.open(
                temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600, dir_fd=self._parent_fd
            )
            created = True
            try:
                stream = os.fdopen(fd, "wb")
            except BaseException:
                os.close(fd)
                raise
            with stream:
                # A 0600 temporary can inherit a default access ACL. chmod can
                # later activate its named entries even with identical mode
                # bits. Reject unsupported inherited metadata before writing
                # staged content or expanding the temporary's permissions.
                if hasattr(os, "listxattr") and os.listxattr(stream.fileno()):
                    raise ValueError(
                        "temporary inherited unsupported extended attributes/ACLs; commit refused"
                    )
                size = 0
                for chunk in self._modified():
                    stream.write(chunk)
                    size += len(chunk)
                stream.flush()
                os.fchown(stream.fileno(), self._info.st_uid, self._info.st_gid)
                os.fchmod(stream.fileno(), stat.S_IMODE(self._info.st_mode))
                os.fsync(stream.fileno())
            self._check()
            phase = "publishing"
            os.replace(
                temporary, self._name, src_dir_fd=self._parent_fd, dst_dir_fd=self._parent_fd
            )
            created = False  # replace removed our temporary name
            os.fsync(self._parent_fd)
            phase = "complete"
        except (Exception, KeyboardInterrupt) as exc:
            if phase == "publishing":
                raise RuntimeError(
                    "file publication may already have occurred; inspect the target before retrying"
                ) from exc
            raise
        finally:
            if created:
                try:
                    os.unlink(temporary, dir_fd=self._parent_fd)
                except FileNotFoundError:
                    pass
        result = CommitResult(self.path, True, count, self.size_bytes, size)
        try:
            self._release()
        except OSError as exc:
            raise RuntimeError(
                "file was committed but editor cleanup failed; inspect before retrying"
            ) from exc
        return _wrap(result)

    @_file_method
    def discard(self) -> None:
        """Discard every staged edit in this object and close it; never save."""
        self._release()

    @_file_method
    def close(self) -> None:
        """Idempotently release descriptors and budget; no commit and no rollback."""
        self._release()

    def _release(self) -> None:
        """Release resources without reentering public method error handling."""
        self._edits.clear()
        self._scope.staged_bytes -= self._staged
        self._staged = 0
        self._scope.files.discard(self)
        failure = None
        for attr in ("_fd", "_parent_fd"):
            descriptor = getattr(self, attr)
            setattr(self, attr, None)
            if descriptor is not None:
                try:
                    os.close(descriptor)
                except OSError as exc:
                    failure = failure or exc
        if failure:
            raise failure


def _file_entry(method):
    """Include direct Python argument binding in the invalidation boundary."""

    @functools.wraps(method)
    def guarded(*args, **kwargs):
        try:
            return method(*args, **kwargs)
        except BaseException as exc:
            try:
                abort_file_operations()
            except BaseException as cleanup:
                exc.add_note(f"file cleanup also failed: {cleanup}")
            raise

    return guarded


@_file_entry
def open_text(path: str, *, writable: bool = False) -> ClassInstance:
    """Open an existing file for this invocation; edits require explicit commit."""
    scope = current_resources()
    if not isinstance(path, str) or not path or "\0" in path:
        raise ValueError("path must be nonempty text without NUL")
    if type(writable) is not bool:
        raise ValueError("writable must be bool")
    if len(scope.files) >= MAX_OPEN_FILES:
        raise ValueError("too many open file editors")
    file = TextFile(path, writable, scope)
    return ClassInstance(
        file,
        eager_attrs={"path", "size_bytes"},
        allowed_methods={
            "read_lines",
            "read_bytes",
            "replace",
            "replace_range",
            "insert",
            "preview",
            "commit",
            "discard",
            "close",
        },
    )


def register_tools(registry):
    """Register the single entry point and its full flat object API."""
    from ..extensions import ToolDescriptor

    registry.add(
        ToolDescriptor(
            name="open_text",
            description="Read and explicitly edit an existing UTF-8 file in bounded chunks.",
            signature="open_text(path: str, *, writable: bool = False) -> TextFile",
            result="TextFile wrapped in ClassInstance. It is an invocation-local editor, not a serializable result. Methods return flat TextSlice, EditPreview or CommitResult wrappers.",
            effects="Reads host files; only explicit commit publishes changes. No autosave, undo, cross-call handles or multi-file atomic transaction.",
            version="1",
            details="All edit positions refer to the opened original version. Reads show original content, not staged content. Any failed file operation invalidates all still-open editors in this PTC invocation, including argument-binding and registry boundary failures caught by Python. Reopen to start fresh. Already committed files are never rolled back.",
            inputs=(
                "path: str — existing regular file, at most 1 GiB; relative to host cwd. Intermediate symlinks and '..' follow filesystem lookup order; final-component symlinks are rejected.",
                "writable: bool = False — enable staged edits. Editing rejects hard links, special permission bits and extended attributes/ACLs. A temporary inheriting directory ACLs is rejected before staged content is written or permissions expand.",
            ),
            return_attributes=(
                "path: str — absolute path at open.",
                "size_bytes: int — original byte count.",
            ),
            return_methods=(
                "read_lines(start: int = 0, count: int = 200) -> TextSlice: original logical lines, zero-based, recognizing LF/CRLF/CR; at most 64 KiB of text.",
                "read_bytes(start: int, count: int) -> TextSlice: UTF-8 byte range; requested partial code points are errors. Budget-cut suffixes are trimmed to complete characters.",
                "replace(old_text: str, new_text: str) -> None: exact unique UTF-8 byte match; old_text nonempty, at most 64 KiB. Empty new_text deletes.",
                "replace_range(start_byte: int, end_byte: int, text: str) -> None: stage original [start,end) bytes at UTF-8 boundaries.",
                "insert(byte_offset: int, text: str) -> None: stage insertion at an original UTF-8 boundary, including EOF.",
                "preview(context_lines: int = 3) -> EditPreview: bounded before/after with aligned zero-based lines and -/+ markers; context range 0–20.",
                "commit() -> CommitResult: validate original identity, stream to a temporary, fsync, replace, fsync directory and close. No edits means no rewrite. Subsequent failures cannot undo a committed file.",
                "discard() -> None; close() -> None: idempotent closure; discard uncommitted edits, never save.",
            ),
            raises=(
                "ValueError/TypeError: invalid ranges/arguments, ambiguous/missing matches, overlapping edits or resource limits; all outstanding edits invalidated.",
                "UnicodeError: invalid UTF-8 or partial character selection; no implicit replacement decoding.",
                "RuntimeError: changed file, read-only/closed editor, or publication uncertainty; inspect after a publication error.",
                "OSError: filesystem/permission/IO failure; no implicit retry.",
            ),
            notes=(
                "TextSlice fields: text, start_byte, end_byte (exclusive), start_line, end_line (exclusive; None for byte reads), truncated. render() -> str returns plain text; to_dict() -> dict detaches data.",
                "EditPreview fields: text, edit_count, truncated; render() -> str and to_dict() -> dict. Previews may truncate, but never truncate the actual edits.",
                "CommitResult fields: path, changed, edit_count, original_size_bytes, size_bytes; render() -> str and to_dict() -> dict.",
                "Result wrappers are flat and invocation-local. Save to_dict() for persistence. Returning a TextFile as a final result is an error; it never commits implicitly.",
                "All still-open editors are closed/discarded on invocation exit. Use one REPL :paste block. No automatic save even when Python catches an edit error.",
                "At most 16 open files, 128 edits per file, 1 MiB aggregate replacement bytes. Insertion points cannot touch another edit boundary; overlapping edits invalidate the batch.",
                "Optimistic device/inode/size/time/metadata checks do not provide an atomic compare-and-swap against uncooperative external writers. Final check-to-rename races remain possible.",
                "A rename/sync interruption may occur after publication: the error explicitly reports uncertainty, never rollback. Multi-file commits are independent.",
            ),
            examples=(
                'file = open_text("example.py")\nprint(file.read_lines(0, 20).render())\nfile.close()',
                'file = open_text("example.py", writable=True)\nfile.replace("old_value", "new_value")\nprint(file.preview().render())\nfile.commit().to_dict()',
                'file = open_text("example.py", writable=True)\npart = file.read_lines(20, 5)\nfile.replace_range(part.start_byte, part.end_byte, "replacement\\n")\nfile.commit().to_dict()',
            ),
        ),
        open_text,
        on_error=abort_file_operations,
    )
