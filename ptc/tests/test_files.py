"""Package-local streamed file editing, explicit publication and Monty integration."""

import os

import pytest

from simplex_ptc import Request, Session, ToolRegistry
from simplex_ptc.resources import execution_resources
from simplex_ptc.tools import files


@pytest.fixture
def scope():
    with execution_resources() as value:
        yield value


def open_file(path, writable=False):
    return files.open_text(str(path), writable=writable).value


def create(tmp_path, data=b"first\r\nsecond\nthird\rlast"):
    path = tmp_path / "file.txt"
    path.write_bytes(data)
    return path


def run(session, code, identity="one"):
    info = session.inspect()
    return session.execute(Request(identity, code, info["epoch"]))


def test_read_lines_and_bytes_preserve_mixed_endings(tmp_path, scope):
    path = create(tmp_path)
    file = open_file(path)
    part = file.read_lines(1, 2).value
    assert part.text == "second\nthird\r"
    assert (part.start_byte, part.end_byte) == (7, 20)
    assert (part.start_line, part.end_line) == (1, 3)
    assert file.read_bytes(7, 6).value.text == "second"
    assert file.read_lines(99, 10).value.text == ""
    assert file.read_lines(1, 0).value.start_byte == 7
    assert len(scope.files) == 1
    file.close()
    assert not scope.files


def test_split_newlines_across_chunks(tmp_path, scope, monkeypatch):
    monkeypatch.setattr(files, "BLOCK_BYTES", 2)
    path = create(tmp_path, b"a\r\nb\rc\nend")
    file = open_file(path)
    assert [file.read_lines(i, 1).value.text for i in range(4)] == ["a\r\n", "b\r", "c\n", "end"]


def test_long_line_and_utf8_budget_are_bounded(tmp_path, scope, monkeypatch):
    monkeypatch.setattr(files, "MAX_OUTPUT_BYTES", 7)
    path = create(tmp_path, "你你你\nnext".encode())
    file = open_file(path)
    part = file.read_lines().value
    assert part.text == "你你"
    assert part.truncated and part.end_byte == 6
    part = file.read_bytes(0, 100).value
    assert part.text == "你你" and part.truncated


@pytest.mark.parametrize("operation", [lambda f: f.read_bytes(1, 3), lambda f: f.read_bytes(0, 2)])
def test_partial_utf8_read_invalidates_editor(tmp_path, scope, operation):
    file = open_file(create(tmp_path, "你好".encode()), True)
    file.insert(0, "prefix")
    with pytest.raises(UnicodeError):
        operation(file)
    with pytest.raises(RuntimeError):
        file.commit()
    assert not scope.files and scope.staged_bytes == 0


def test_stage_original_offsets_then_commit(tmp_path, scope):
    path = create(tmp_path, b"abc def ghi\n")
    os.chmod(path, 0o640)
    file = open_file(path, True)
    file.replace_range(4, 7, "LONGER")
    file.replace_range(8, 11, "x")
    assert path.read_bytes() == b"abc def ghi\n"
    assert file.read_lines().value.text == "abc def ghi\n"
    receipt = file.commit().value
    assert path.read_bytes() == b"abc LONGER x\n"
    assert receipt.changed and receipt.edit_count == 2
    assert receipt.size_bytes == len(path.read_bytes())
    assert path.stat().st_mode & 0o777 == 0o640
    assert not scope.files and scope.staged_bytes == 0
    with pytest.raises(RuntimeError):
        file.insert(0, "no")


def test_replace_across_blocks_and_unique_overlapping_match(tmp_path, scope, monkeypatch):
    monkeypatch.setattr(files, "BLOCK_BYTES", 4)
    path = create(tmp_path, b"123abcdef890")
    file = open_file(path, True)
    file.replace("abcdef", "X")
    file.commit()
    assert path.read_bytes() == b"123X890"
    path.write_bytes(b"aaaa")
    file = open_file(path, True)
    with pytest.raises(ValueError, match="exactly once"):
        file.replace("aaa", "x")
    with pytest.raises(RuntimeError):
        file.commit()
    assert path.read_bytes() == b"aaaa"


@pytest.mark.parametrize(
    "fail",
    [
        lambda f: f.replace("missing", "x"),
        lambda f: f.replace_range(0, 999, "x"),
        lambda f: f.replace_range(1, 3, "overlap"),
        lambda f: f.insert(0, "overlap"),
        lambda f: f.preview(21),
    ],
)
def test_invalid_condition_discards_entire_batch(tmp_path, scope, fail):
    path = create(tmp_path, b"abcdef")
    file = open_file(path, True)
    file.replace_range(0, 2, "staged")
    with pytest.raises((ValueError, RuntimeError)):
        fail(file)
    with pytest.raises(RuntimeError):
        file.commit()
    assert path.read_bytes() == b"abcdef"
    assert scope.staged_bytes == 0


def test_failure_also_invalidates_other_outstanding_editors(tmp_path, scope):
    a = create(tmp_path, b"original")
    b = tmp_path / "other"
    b.write_text("other")
    fa, fb = open_file(a, True), open_file(b, True)
    fa.insert(0, "a")
    fb.insert(0, "b")
    with pytest.raises(ValueError):
        fb.replace("missing", "c")
    with pytest.raises(RuntimeError):
        fa.commit()
    assert a.read_text() == "original" and b.read_text() == "other"
    # Explicitly reopening starts a fresh batch after a caught failure.
    fresh = open_file(a, True)
    fresh.insert(0, "new ")
    fresh.commit()
    assert a.read_text() == "new original"


def test_exit_and_discard_never_save(tmp_path):
    path = create(tmp_path, b"original")
    with execution_resources():
        file = open_file(path, True)
        file.replace("original", "changed")
    assert file._fd is None
    assert path.read_bytes() == b"original"
    with execution_resources():
        file = open_file(path, True)
        file.insert(0, "changed")
        file.discard()
        file.close()
    assert path.read_bytes() == b"original"


def test_conflict_external_write_and_replacement(tmp_path, scope):
    path = create(tmp_path, b"original")
    file = open_file(path, True)
    file.replace("original", "ours")
    path.write_bytes(b"external")
    with pytest.raises(RuntimeError, match="changed"):
        file.commit()
    assert path.read_bytes() == b"external"
    file = open_file(path, True)
    file.insert(0, "ours")
    replacement = tmp_path / "replacement"
    replacement.write_bytes(b"replaced")
    os.replace(replacement, path)
    with pytest.raises(RuntimeError, match="changed"):
        file.commit()
    assert path.read_bytes() == b"replaced"
    assert not list(tmp_path.glob(".simplex-edit-*"))


def test_invalid_utf8_outside_selected_region_blocks_publication(tmp_path, scope):
    path = create(tmp_path, b"valid\n\xffinvalid")
    file = open_file(path, True)
    assert file.read_lines(0, 1).value.text == "valid\n"
    file.replace("valid\n", "new\n")
    with pytest.raises(UnicodeError):
        file.commit()
    assert path.read_bytes() == b"valid\n\xffinvalid"
    assert not list(tmp_path.glob(".simplex-edit-*"))


def test_preview_has_aligned_markers_original_and_modified_lines(tmp_path, scope):
    path = create(tmp_path, b"zero\none\ntwo\nthree\n")
    file = open_file(path, True)
    file.replace("one", "ONE\ninserted")
    preview = file.preview(1).value
    assert "- 1 | one" in preview.text
    assert "+ 1 | ONE" in preview.text
    assert "+ 2 | inserted" in preview.text
    assert "  0 | zero" in preview.text
    assert "  3 | two" in preview.text
    assert not preview.truncated
    assert path.read_bytes() == b"zero\none\ntwo\nthree\n"


def test_large_preview_is_bounded_and_commit_is_not_truncated(tmp_path, scope):
    path = create(tmp_path, b"old\n")
    file = open_file(path, True)
    text = "long line " * 20000
    file.replace("old", text)
    preview = file.preview().value
    assert len(preview.text.encode()) <= files.MAX_OUTPUT_BYTES
    assert preview.truncated
    file.commit()
    assert path.read_text() == text + "\n"


def test_empty_and_eof_insertion_and_noop(tmp_path, scope):
    path = create(tmp_path, b"")
    file = open_file(path, True)
    original = path.stat().st_ino
    assert not file.commit().value.changed
    assert path.stat().st_ino == original
    file = open_file(path, True)
    file.insert(0, "hello")
    assert "[EOF]" in file.preview().value.text
    file.commit()
    assert path.read_text() == "hello"


def test_prepublication_write_failure_does_not_touch_target(tmp_path, scope, monkeypatch):
    path = create(tmp_path, b"original")
    file = open_file(path, True)
    file.replace("original", "changed")
    monkeypatch.setattr(os, "fsync", lambda *args: (_ for _ in ()).throw(OSError("disk failure")))
    with pytest.raises(OSError):
        file.commit()
    assert path.read_bytes() == b"original"
    assert not list(tmp_path.glob(".simplex-edit-*"))
    assert not scope.files


def test_postpublication_sync_failure_is_explicit_uncertainty(tmp_path, scope, monkeypatch):
    path = create(tmp_path, b"original")
    file = open_file(path, True)
    file.replace("original", "changed")
    sync = os.fsync

    def fail_directory(fd):
        if fd == file._parent_fd:
            raise OSError("directory sync failure")
        sync(fd)

    monkeypatch.setattr(os, "fsync", fail_directory)
    with pytest.raises(RuntimeError, match="publication may already have occurred"):
        file.commit()
    assert path.read_bytes() == b"changed"
    assert not scope.files


def test_readonly_and_unsupported_files(tmp_path, scope):
    path = create(tmp_path)
    with pytest.raises(RuntimeError, match="read-only"):
        open_file(path).insert(0, "x")
    link = tmp_path / "symlink"
    link.symlink_to(path)
    with pytest.raises(ValueError, match="regular file"):
        open_file(link)
    hard = tmp_path / "hard"
    os.link(path, hard)
    with pytest.raises(ValueError, match="single-link"):
        open_file(path, True)
    assert not scope.files


def test_monty_flat_objects_commit_and_manual(tmp_path):
    path = create(tmp_path, b"old\n")
    session = Session(tmp_path / "state", ToolRegistry.load(["simplex_ptc.tools.files"]))
    source = f"""f = open_text({str(path)!r}, writable=True)
p = f.read_lines()
assert p.text == "old\\n"
f.replace("old", "new")
print(f.preview().render())
f.commit()
"""
    result = run(session, source)
    assert result["status"] == "completed", result
    assert result["result"]["changed"]
    assert path.read_bytes() == b"new\n"
    second = run(session, 'manual("open_text").to_dict()', "two")
    assert second["result"]["return_methods"]


def test_caught_failure_cannot_commit_via_monty(tmp_path):
    path = create(tmp_path, b"old")
    session = Session(tmp_path / "state", ToolRegistry.load(["simplex_ptc.tools.files"]))
    result = run(
        session,
        f"""f = open_text({str(path)!r}, writable=True)
f.insert(0, "bad")
try:
    f.replace("missing", "new")
except ValueError:
    pass
f.commit()
""",
    )
    assert result["status"] == "failed", result
    assert path.read_bytes() == b"old"


def test_python_failure_discards_pending_but_not_committed_file(tmp_path):
    path = create(tmp_path, b"old")
    session = Session(tmp_path / "state", ToolRegistry.load(["simplex_ptc.tools.files"]))
    prefix = f'f = open_text({str(path)!r}, writable=True)\nf.replace("old", "new")\n'
    result = run(session, prefix + 'raise ValueError("later")')
    assert result["status"] == "failed"
    assert path.read_bytes() == b"old"
    result = run(session, prefix + 'f.commit()\nraise ValueError("later")', "two")
    assert result["status"] == "failed"
    assert path.read_bytes() == b"new"


def test_no_open_handle_can_be_final_result_or_reused_after_restore(tmp_path):
    path = create(tmp_path)
    session = Session(tmp_path / "state", ToolRegistry.load(["simplex_ptc.tools.files"]))
    result = run(session, f"open_text({str(path)!r})")
    assert result["error"]["code"] == "conversion_error"
    first = run(
        session, f"f = open_text({str(path)!r})\nsaved = f.read_lines().to_dict()\nsaved", "two"
    )
    assert first["status"] == "completed"
    later = run(session, "saved", "three")
    assert later["result"] == first["result"]
    final = run(session, "f.read_lines()", "four")
    assert final["status"] == "failed"


def test_unchanged_replacement_does_not_rewrite(tmp_path, scope):
    path = create(tmp_path, b"unchanged")
    file = open_file(path, True)
    original = path.stat().st_ino
    file.replace("unchanged", "unchanged")
    assert not file.commit().value.changed
    assert path.stat().st_ino == original


def test_budget_exact_line_does_not_claim_next_line(tmp_path, scope, monkeypatch):
    monkeypatch.setattr(files, "MAX_OUTPUT_BYTES", 2)
    file = open_file(create(tmp_path, b"a\nb\n"))
    result = file.read_lines(0, 2).value
    assert result.text == "a\n"
    assert result.truncated
    assert result.end_line == 1


def test_large_file_streaming_memory_and_scan_bound(tmp_path, scope, monkeypatch):
    import tracemalloc

    size = 4 * 1024 * 1024
    path = create(tmp_path, b"a" * (size - 6) + b"NEEDLE")
    original = os.pread
    observed = 0

    def bounded(fd, count, offset):
        nonlocal observed
        assert count <= files.BLOCK_BYTES
        data = original(fd, count, offset)
        observed += len(data)
        return data

    monkeypatch.setattr(os, "pread", bounded)
    tracemalloc.start()
    try:
        file = open_file(path, True)
        file.replace("NEEDLE", "new")
        file.commit()
        _, peak = tracemalloc.get_traced_memory()
    finally:
        tracemalloc.stop()
    assert observed <= size * 2 + 2
    assert peak < 2 * 1024 * 1024
    with path.open("rb") as stream:
        stream.seek(-3, os.SEEK_END)
        assert stream.read() == b"new"


def test_staged_budget_is_shared_and_released(tmp_path, scope, monkeypatch):
    import simplex_ptc.resources as resources

    monkeypatch.setattr(resources, "MAX_STAGED_BYTES", 5)
    a = create(tmp_path, b"a")
    b = tmp_path / "b"
    b.write_text("b")
    first, second = open_file(a, True), open_file(b, True)
    first.insert(0, "1234")
    with pytest.raises(ValueError, match="budget"):
        second.insert(0, "56")
    assert not scope.files and scope.staged_bytes == 0
    with pytest.raises(RuntimeError):
        first.commit()
    assert a.read_bytes() == b"a" and b.read_bytes() == b"b"


def test_parent_path_retarget_does_not_redirect_publication(tmp_path, scope):
    first, second = tmp_path / "first", tmp_path / "second"
    first.mkdir()
    second.mkdir()
    (first / "data").write_text("first")
    (second / "data").write_text("second")
    alias = tmp_path / "alias"
    alias.symlink_to(first, target_is_directory=True)
    file = open_file(alias / "data", True)
    file.insert(0, "new")
    alias.unlink()
    alias.symlink_to(second, target_is_directory=True)
    with pytest.raises(RuntimeError, match="changed"):
        file.commit()
    assert (first / "data").read_text() == "first"
    assert (second / "data").read_text() == "second"


def test_change_during_copy_prevents_commit(tmp_path, scope, monkeypatch):
    path = create(tmp_path, b"original")
    file = open_file(path, True)
    file.insert(0, "new")
    original = file._modified

    def concurrent_write():
        yield from original()
        path.write_bytes(b"external")

    monkeypatch.setattr(file, "_modified", concurrent_write)
    with pytest.raises(RuntimeError, match="changed"):
        file.commit()
    assert path.read_bytes() == b"external"
    assert not list(tmp_path.glob(".simplex-edit-*"))


def test_replace_failure_and_post_replace_interrupt_are_explicit(tmp_path, scope, monkeypatch):
    path = create(tmp_path, b"original")
    file = open_file(path, True)
    file.insert(0, "new")
    original = os.replace

    def replace_then_interrupt(*args, **kwargs):
        original(*args, **kwargs)
        raise KeyboardInterrupt()

    monkeypatch.setattr(os, "replace", replace_then_interrupt)
    with pytest.raises(RuntimeError, match="publication may already have occurred"):
        file.commit()
    assert path.read_bytes() == b"neworiginal"
    assert not scope.files


def test_temporary_collision_is_not_deleted(tmp_path, scope, monkeypatch):
    from types import SimpleNamespace

    path = create(tmp_path, b"original")
    file = open_file(path, True)
    file.insert(0, "new")
    collision = tmp_path / ".simplex-edit-collision"
    collision.write_text("not owned by the editor")
    monkeypatch.setattr(files.uuid, "uuid4", lambda: SimpleNamespace(hex="collision"))
    with pytest.raises(FileExistsError):
        file.commit()
    assert collision.read_text() == "not owned by the editor"
    assert path.read_text() == "original"


def test_all_editors_close_even_when_one_cleanup_reports_error(tmp_path, monkeypatch):
    from simplex_ptc.resources import current_resources

    path = create(tmp_path)
    original = files.TextFile.close
    seen = []

    def fail_after_close(self):
        original(self)
        seen.append(self)
        raise OSError("close reported failure")

    with pytest.raises(OSError, match="close reported failure"):
        with execution_resources():
            first = open_file(path)
            second = open_file(path)
            monkeypatch.setattr(files.TextFile, "close", fail_after_close)
    assert set(seen) == {first, second}
    assert first._fd is None and second._fd is None
    with pytest.raises(RuntimeError, match="active PTC execution"):
        current_resources()


def test_context_cleanup_preserves_original_error(tmp_path, monkeypatch):
    path = create(tmp_path)
    original = files.TextFile.close

    def fail_after_close(self):
        original(self)
        raise OSError("cleanup error")

    with pytest.raises(ValueError, match="primary") as error:
        with execution_resources():
            open_file(path)
            monkeypatch.setattr(files.TextFile, "close", fail_after_close)
            raise ValueError("primary")
    assert any("cleanup error" in note for note in error.value.__notes__)


def test_open_failure_discards_existing_edits(tmp_path, scope):
    path = create(tmp_path, b"original")
    file = open_file(path, True)
    file.insert(0, "new")
    with pytest.raises(FileNotFoundError):
        open_file(tmp_path / "missing")
    with pytest.raises(RuntimeError):
        file.commit()
    assert path.read_bytes() == b"original"


def test_prepublication_cancellation_removes_temporary(tmp_path, scope, monkeypatch):
    path = create(tmp_path, b"original")
    file = open_file(path, True)
    file.insert(0, "new")
    monkeypatch.setattr(os, "fsync", lambda *args: (_ for _ in ()).throw(KeyboardInterrupt()))
    with pytest.raises(KeyboardInterrupt):
        file.commit()
    assert path.read_bytes() == b"original"
    assert not list(tmp_path.glob(".simplex-edit-*"))


def test_preview_aligns_different_line_number_widths(tmp_path, scope):
    path = create(tmp_path, "".join(f"line {i}\n" for i in range(15)).encode())
    file = open_file(path, True)
    file.replace("line 9\n", "new\nmore\n")
    text = file.preview(1).value.text
    numbered = [line for line in text.splitlines() if " | " in line]
    assert len({line.index("|") for line in numbered}) == 1
    assert "-  9 | line 9" in text
    assert "+ 10 | more" in text


def test_nonregular_files_and_open_limit_do_not_leave_handles(tmp_path, scope, monkeypatch):
    fifo = tmp_path / "fifo"
    os.mkfifo(fifo)
    with pytest.raises(ValueError, match="regular file"):
        open_file(fifo)
    with pytest.raises(ValueError, match="regular file"):
        open_file(tmp_path)
    path = create(tmp_path)
    monkeypatch.setattr(files, "MAX_OPEN_FILES", 1)
    first = open_file(path, True)
    first.insert(0, "pending")
    with pytest.raises(ValueError, match="too many open"):
        open_file(path)
    assert first._fd is None and not scope.files and scope.staged_bytes == 0


def test_utf8_edit_boundaries_and_clipped_preview_context(tmp_path, scope):
    path = create(tmp_path, "你好\n".encode())
    file = open_file(path, True)
    with pytest.raises(ValueError, match="splits a UTF-8"):
        file.insert(1, "bad")
    assert path.read_bytes() == "你好\n".encode()
    path.write_text("a" * 5000 + "\nchange\n")
    file = open_file(path, True)
    file.replace("change", "changed")
    preview = file.preview(1).value
    assert preview.truncated
    assert "[line truncated]" in preview.text
