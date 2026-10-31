"""Package-local tests: real Monty, fault-injected storage, no worker/Hub."""

import json
import os
import subprocess
import sys

import pytest

from simplex_ptc import PTCError, Request, Session, ToolDescriptor, ToolRegistry
from simplex_ptc.protocol import MAX_RESPONSE_BYTES, encode
from simplex_ptc.storage import Storage


def execute(session, identity, source):
    receipt = session.storage.read("checkpoint")
    epoch = receipt["epoch"] if receipt else None
    # An initial-request retry uses the exact original request digest.
    if receipt and (receipt.get("receipt") or {}).get("execution_id") == identity:
        from simplex_ptc.session import digest

        if receipt["receipt"]["request_digest"] == digest(
            encode({"source": source, "epoch": None})
        ):
            epoch = None
    return session.execute(Request(identity, source, epoch))


def cli(path, identity, source):
    return subprocess.run(
        [sys.executable, "-m", "simplex_ptc", "execute", "--state-directory", str(path)],
        input=json.dumps(
            {
                "execution_id": identity,
                "source": source,
                "epoch": (
                    json.loads((path / "checkpoint").read_text())["epoch"]
                    if (path / "checkpoint").exists()
                    else None
                ),
            }
        ),
        text=True,
        capture_output=True,
        timeout=15,
    )


def test_state_and_functions_survive_processes(tmp_path):
    first = cli(tmp_path, "first", "x = 40\ndef add(y):\n    return x + y\nadd(1)")
    assert first.returncode == 0, first.stderr
    second = cli(tmp_path, "second", "add(2)")
    assert second.returncode == 0, second.stderr
    assert json.loads(second.stdout)["result"] == 42
    assert json.loads(second.stdout)["revision"] == 2


def test_failure_does_not_commit_changes(tmp_path):
    session = Session(tmp_path)
    execute(session, "one", "x = 1")
    failed = execute(session, "two", "x = 2\nraise ValueError('bad')")
    assert failed["status"] == "failed"
    assert failed["revision"] == 1
    assert not failed["committed"]
    assert execute(session, "three", "x")["result"] == 1


def test_conversion_failure_preserves_previous_state(tmp_path):
    session = Session(tmp_path)
    execute(session, "one", "x = 1")
    failed = execute(session, "two", "x = 2\nb'bytes'")
    assert failed["error"]["code"] == "conversion_error"
    assert execute(session, "three", "x")["result"] == 1


def test_duplicate_identity_returns_saved_receipt(tmp_path):
    session = Session(tmp_path)
    first = execute(session, "one", "x = 1\nx")
    assert execute(session, "one", "x = 1\nx") == first
    with pytest.raises(PTCError, match="different source"):
        execute(session, "one", "x = 2")
    assert session.inspect()["revision"] == 1


def test_failed_receipt_is_also_replayed(tmp_path):
    session = Session(tmp_path)
    result = execute(session, "one", "1 / 0")
    assert execute(session, "one", "1 / 0") == result
    assert session.inspect()["revision"] == 0


def test_syntax_failure_is_classified(tmp_path):
    result = execute(Session(tmp_path), "syntax", "if :")
    assert result["error"]["code"] == "compile_error"


def test_print_and_large_results_are_valid_bounded_json(tmp_path):
    response = execute(Session(tmp_path), "one", "print('界' * 20000)\n'界' * 20000")
    assert response["committed"]
    assert response["result_truncated"]
    assert response["output_omitted_bytes"] > 0
    assert len(encode(response)) <= MAX_RESPONSE_BYTES
    json.loads(encode(response))


def test_output_escaping_is_budgeted(tmp_path):
    response = execute(Session(tmp_path), "one", "print('\\x00' * 18000)\n42")
    assert len(encode(response)) <= MAX_RESPONSE_BYTES
    assert response["output_omitted_bytes"] > 0
    assert response["result"] == 42


def test_default_environment_cannot_read_host_file(tmp_path):
    secret = tmp_path / "secret"
    secret.write_text("not exposed")
    response = execute(Session(tmp_path / "state"), "one", f"open({str(secret)!r}).read()")
    assert response["status"] == "failed"
    assert "not exposed" not in str(response)


def test_reset_changes_epoch_and_preserves_external_files(tmp_path):
    session = Session(tmp_path)
    execute(session, "one", "x = 1")
    old = session.inspect()
    external = tmp_path / "external"
    external.write_text("keep")
    session.reset(old["epoch"])
    new = session.inspect()
    assert new["epoch"] != old["epoch"]
    assert new["session_id"] == old["session_id"]
    assert external.read_text() == "keep"
    assert execute(session, "two", "x")["status"] == "failed"
    with pytest.raises(PTCError, match="epoch"):
        session.reset(old["epoch"])


def test_cross_process_lock_releases_after_owner_exits(tmp_path):
    with Storage(tmp_path).locked():
        result = cli(tmp_path, "one", "1")
        assert result.returncode == 2
        assert json.loads(result.stdout)["error"]["code"] == "busy"
    assert cli(tmp_path, "one", "1").returncode == 0


def test_write_failure_blocks_new_execution_until_recovery(tmp_path, monkeypatch):
    session = Session(tmp_path)
    execute(session, "one", "x = 1")
    write = session.storage.write

    def fail(name, value):
        if name == "checkpoint":
            raise OSError("disk full")
        write(name, value)

    monkeypatch.setattr(session.storage, "write", fail)
    with pytest.raises(OSError):
        execute(session, "two", "x = 2")
    monkeypatch.setattr(session.storage, "write", write)
    info = session.inspect()
    assert info["recovery_required"]
    assert info["revision"] == 1
    with pytest.raises(PTCError, match="acknowledged"):
        execute(session, "three", "x")
    with pytest.raises(PTCError):
        session.recover(info["epoch"], "wrong")
    session.recover(info["epoch"], "two")
    assert execute(session, "two", "x = 2")["error"]["code"] == "interrupted"
    assert execute(session, "three", "x")["result"] == 1


def test_commit_then_unlink_failure_recovers_without_reexecution(tmp_path, monkeypatch):
    session = Session(tmp_path)
    original = session.storage.clear_pending
    monkeypatch.setattr(session.storage, "clear_pending", lambda: (_ for _ in ()).throw(OSError()))
    with pytest.raises(OSError):
        execute(session, "one", "x = 1\nx")
    monkeypatch.setattr(session.storage, "clear_pending", original)
    assert not session.inspect()["recovery_required"]
    assert execute(session, "one", "x = 1\nx")["result"] == 1
    assert execute(session, "two", "x + 1")["result"] == 2


def test_atomic_replace_failure_keeps_original_checkpoint(tmp_path, monkeypatch):
    session = Session(tmp_path)
    execute(session, "one", "1")
    before = (tmp_path / "checkpoint").read_bytes()
    monkeypatch.setattr(
        os, "replace", lambda *args: (_ for _ in ()).throw(OSError("replace failed"))
    )
    with pytest.raises(OSError):
        session.storage.write("checkpoint", {"changed": True})
    assert (tmp_path / "checkpoint").read_bytes() == before
    assert list(tmp_path.glob(".checkpoint-*")) == []


def test_corrupt_and_incompatible_state_fail_without_overwrite(tmp_path):
    session = Session(tmp_path)
    execute(session, "one", "1")
    path = tmp_path / "checkpoint"
    valid = json.loads(path.read_text())
    changed = dict(valid, dump_sha256="invalid")
    path.write_text(json.dumps(changed))
    before = path.read_bytes()
    with pytest.raises(PTCError, match="invalid checkpoint"):
        execute(session, "two", "2")
    assert path.read_bytes() == before
    path.write_text(json.dumps(dict(valid, format=999)))
    with pytest.raises(PTCError, match="changed"):
        execute(session, "two", "2")


def test_extension_rebind_and_external_effect_not_rolled_back(tmp_path):
    calls = []
    registry = ToolRegistry()
    registry.add(
        ToolDescriptor("record", "Record", "record(x: int) -> int", "int", "write", "1"),
        lambda value: calls.append(value) or value,
    )
    session = Session(tmp_path, registry)
    assert execute(session, "one", "x = record(1)")["committed"]
    assert execute(session, "two", "x = record(2)\nraise ValueError('fail')")["status"] == "failed"
    assert calls == [1, 2]
    assert execute(session, "three", "x")["result"] == 1
    with pytest.raises(PTCError, match="changed"):
        Session(tmp_path).execute(Request("four", "x"))


def test_registry_rejects_duplicate_and_async_handlers():
    registry = ToolRegistry()
    descriptor = ToolDescriptor("f", "Function", "f() -> int", "int", "none", "1")
    registry.add(descriptor, lambda: 1)
    with pytest.raises(PTCError):
        registry.add(descriptor, lambda: 2)

    async def handler():
        return 1

    with pytest.raises(PTCError):
        ToolRegistry().add(descriptor, handler)


def test_cli_invalid_request_does_not_create_state(tmp_path):
    path = tmp_path / "state"
    result = subprocess.run(
        [sys.executable, "-m", "simplex_ptc", "execute", "--state-directory", str(path)],
        input="not json",
        text=True,
        capture_output=True,
        timeout=10,
    )
    assert result.returncode == 2
    assert json.loads(result.stdout)["error"]["code"] == "invalid_request"
    assert not path.exists()


def test_runtime_limit_discards_uncommitted_changes(tmp_path, monkeypatch):
    from simplex_ptc import runtime

    monkeypatch.setitem(runtime.LIMITS, "max_feed_duration_secs", 0.05)
    monkeypatch.setitem(runtime.LIMITS, "max_turn_duration_secs", 0.05)
    session = Session(tmp_path)
    execute(session, "one", "x = 1")
    result = execute(session, "two", "x = 2\nwhile True:\n    pass")
    assert result["status"] == "failed"
    assert execute(session, "three", "x")["result"] == 1


def test_stale_epoch_rejected_after_reset(tmp_path):
    session = Session(tmp_path)
    first = execute(session, "one", "1")
    session.reset(first["epoch"])
    for epoch in (None, first["epoch"]):
        with pytest.raises(PTCError, match="current session epoch"):
            session.execute(Request("old", "2", epoch))
    assert execute(session, "new", "3")["result"] == 3


def test_independent_sessions_do_not_share_state(tmp_path):
    execute(Session(tmp_path / "a"), "one", "x = 123")
    assert execute(Session(tmp_path / "b"), "one", "x")["status"] == "failed"


def test_pending_before_runtime_crash_requires_acknowledgement(tmp_path, monkeypatch):
    from simplex_ptc import runtime

    session = Session(tmp_path)

    def crash(*args):
        raise SystemExit(9)

    with monkeypatch.context() as patch:
        patch.setattr(runtime, "run", crash)
        with pytest.raises(SystemExit):
            execute(session, "one", "1")
    info = session.inspect()
    assert info["recovery_required"]
    assert info["revision"] == 0
    session.recover(info["epoch"], "one")
    assert execute(session, "two", "2")["result"] == 2


def test_cancelled_execution_records_failure(tmp_path, monkeypatch):
    from simplex_ptc import runtime

    session = Session(tmp_path)
    execute(session, "one", "x = 7")

    def cancel(*args):
        raise KeyboardInterrupt()

    with monkeypatch.context() as patch:
        patch.setattr(runtime, "run", cancel)
        assert execute(session, "two", "x = 8")["error"]["code"] == "cancelled"
    assert not session.inspect()["recovery_required"]
    assert execute(session, "three", "x")["result"] == 7


def test_post_replace_sync_failure_keeps_committed_receipt(tmp_path, monkeypatch):
    session = Session(tmp_path)
    execute(session, "one", "x = 1")
    sync = session.storage.sync_directory
    count = 0

    def fail_after_checkpoint():
        nonlocal count
        count += 1
        if count == 2:  # pending sync succeeded; checkpoint already replaced
            raise OSError("directory fsync failed")
        sync()

    with monkeypatch.context() as patch:
        patch.setattr(session.storage, "sync_directory", fail_after_checkpoint)
        with pytest.raises(OSError):
            execute(session, "two", "x = 2")
    assert not session.inspect()["recovery_required"]
    assert execute(session, "two", "x = 2")["committed"]
    assert execute(session, "three", "x")["result"] == 2


def test_reset_settles_old_committed_marker_before_new_epoch(tmp_path, monkeypatch):
    session = Session(tmp_path)

    def fail():
        raise OSError("unlink failed")

    with monkeypatch.context() as patch:
        patch.setattr(session.storage, "clear_pending", fail)
        with pytest.raises(OSError):
            execute(session, "one", "1")
    epoch = session.inspect()["epoch"]
    original = session.storage.write

    def published_then_failed(name, value):
        original(name, value)
        raise OSError("response interrupted")

    with monkeypatch.context() as patch:
        patch.setattr(session.storage, "write", published_then_failed)
        with pytest.raises(OSError):
            session.reset(epoch)
    assert not session.inspect()["recovery_required"]
    assert session.inspect()["epoch"] != epoch
    assert execute(session, "two", "2")["result"] == 2


def test_checkpoint_limit_records_failure_with_old_heap(tmp_path, monkeypatch):
    import simplex_ptc.session as session_module

    session = Session(tmp_path)
    execute(session, "one", "x = 1")
    with monkeypatch.context() as patch:
        patch.setattr(session_module, "MAX_CHECKPOINT_BYTES", 1)
        result = execute(session, "two", "x = 2")
    assert result["error"]["code"] == "storage_limit"
    assert not result["committed"]
    assert not session.inspect()["recovery_required"]
    assert execute(session, "three", "x")["result"] == 1


def test_no_coercion_of_non_string_mapping_keys(tmp_path):
    result = execute(Session(tmp_path), "one", "{1: 'value'}")
    assert result["error"]["code"] == "conversion_error"


def test_extension_discovery_uses_same_registry_and_rejects_missing(monkeypatch):
    import types

    module = types.ModuleType("ptc_fixture")
    module.register_tools = lambda registry: registry.add(
        ToolDescriptor("constant", "Constant", "constant() -> int", "int", "none", "1"),
        lambda: 5,
    )
    monkeypatch.setitem(sys.modules, "ptc_fixture", module)
    registry = ToolRegistry.load(["ptc_fixture"])
    assert registry.manifest()[0]["name"] == "constant"
    assert registry.bindings()["constant"]() == 5
    with pytest.raises(PTCError, match="cannot register"):
        ToolRegistry.load(["simplex_ptc_nonexistent_extension"])


def test_cli_capabilities_empty_by_default():
    result = subprocess.run(
        [sys.executable, "-m", "simplex_ptc", "capabilities"],
        text=True,
        capture_output=True,
        timeout=10,
    )
    assert result.returncode == 0
    assert json.loads(result.stdout)["tools"] == []


def test_cleanup_removes_only_owned_staging_files(tmp_path):
    stale = tmp_path / ".checkpoint-stale"
    stale.write_text("incomplete")
    other = tmp_path / "keep.txt"
    other.write_text("keep")
    Session(tmp_path).inspect()
    assert not stale.exists()
    assert other.read_text() == "keep"


@pytest.mark.parametrize(
    "value",
    [
        None,
        {},
        {"execution_id": "", "source": "1"},
        {"execution_id": "one", "source": 5},
        {"execution_id": "one", "source": "1", "module": "bad"},
        {"execution_id": "one", "source": "\ud800"},
        {"execution_id": "one", "source": "1", "epoch": []},
    ],
)
def test_invalid_requests(value):
    with pytest.raises(PTCError):
        Request.parse(value)


def test_interruption_during_response_preparation_does_not_commit_new_heap(tmp_path, monkeypatch):
    import simplex_ptc.session as session_module

    session = Session(tmp_path)
    execute(session, "one", "x = 1")
    original = session_module.bound_response
    calls = 0

    def interrupt(response):
        nonlocal calls
        calls += 1
        if calls == 2:
            raise KeyboardInterrupt()
        return original(response)

    with monkeypatch.context() as patch:
        patch.setattr(session_module, "bound_response", interrupt)
        response = execute(session, "two", "x = 2")
    assert response["error"]["code"] == "cancelled"
    assert not response["committed"]
    assert response["revision"] == 1
    assert execute(session, "three", "x")["result"] == 1


def test_pending_publication_failure_prevents_runtime(tmp_path, monkeypatch):
    from simplex_ptc import runtime

    session = Session(tmp_path)
    execute(session, "one", "x = 1")
    original = session.storage.write

    def fail_pending(name, value):
        if name == "execution.pending":
            raise OSError("cannot publish pending")
        original(name, value)

    def must_not_run(*args):
        pytest.fail("executed before pending was durable")

    monkeypatch.setattr(session.storage, "write", fail_pending)
    monkeypatch.setattr(runtime, "run", must_not_run)
    with pytest.raises(OSError):
        execute(session, "two", "x = 2")
    assert session.inspect()["revision"] == 1


def test_foreign_pending_marker_is_not_recovered_into_current_epoch(tmp_path):
    session = Session(tmp_path)
    execute(session, "one", "1")
    session.storage.write(
        "execution.pending",
        {
            "epoch": "foreign",
            "base_revision": 1,
            "execution_id": "other",
            "request_digest": "other",
        },
    )
    with pytest.raises(PTCError, match="does not match checkpoint"):
        session.recover("foreign", "other")


def test_replay_retries_durability_fence_without_repeating_effects(tmp_path, monkeypatch):
    effects = []
    registry = ToolRegistry()
    registry.add(
        ToolDescriptor("record", "Record", "record(x: int) -> int", "int", "write", "1"),
        lambda value: effects.append(value) or value,
    )
    session = Session(tmp_path, registry)
    execute(session, "one", "x = 1")
    request = Request("two", "x = record(2)\nx", session.inspect()["epoch"])
    original = session.storage.sync_directory
    attempts = 0

    def persistent_failure():
        nonlocal attempts
        attempts += 1
        if attempts >= 2:
            raise OSError("persistent directory sync failure")
        original()

    with monkeypatch.context() as patch:
        patch.setattr(session.storage, "sync_directory", persistent_failure)
        with pytest.raises(OSError):
            session.execute(request)
        assert effects == [2]
        before = attempts
        assert not session.inspect()["recovery_required"]
        assert attempts == before  # inspection does not repair or acknowledge durability
        for _ in range(3):
            with pytest.raises(OSError):
                session.execute(request)
            before += 1
            assert attempts == before
            assert (tmp_path / "execution.pending").exists()
            assert effects == [2]
    result = session.execute(request)
    assert result["committed"] and result["result"] == 2
    assert result["revision"] == 2
    assert not (tmp_path / "execution.pending").exists()
    assert effects == [2]


def test_replay_reconciles_failed_unlink_sync_without_visible_marker(tmp_path, monkeypatch):
    session = Session(tmp_path)
    first = execute(session, "one", "1")
    request = Request("two", "2", first["epoch"])
    original = session.storage.sync_directory
    attempts = 0

    def fail_after_unlink():
        nonlocal attempts
        attempts += 1
        if attempts >= 3:
            raise OSError("unlink directory sync failure")
        original()

    with monkeypatch.context() as patch:
        patch.setattr(session.storage, "sync_directory", fail_after_unlink)
        with pytest.raises(OSError):
            session.execute(request)
        assert not (tmp_path / "execution.pending").exists()
        with pytest.raises(OSError):
            session.execute(request)
        assert attempts == 4
    assert session.execute(request)["result"] == 2


@pytest.mark.parametrize("name", ["len", "print", "open", "int", "ValueError"])
def test_builtin_tool_names_are_rejected_without_advertising(name):
    registry = ToolRegistry()
    with pytest.raises(PTCError, match="reserved for Python builtins"):
        registry.add(ToolDescriptor(name, "Test", f"{name}()", "int", "none", "1"), lambda: 42)
    assert registry.manifest() == []
    assert registry.bindings() == {}


def test_distinct_host_name_and_builtins_work_after_restore(tmp_path):
    effects = []
    registry = ToolRegistry()
    registry.add(
        ToolDescriptor("tool_length", "Length", "tool_length(x) -> int", "int", "none", "1"),
        lambda value: effects.append(value) or 42,
    )
    session = Session(tmp_path, registry)
    source = "print('hello')\n[tool_length([1, 2]), len([1, 2])]"
    for identity in ("first", "restored"):
        result = execute(session, identity, source)
        assert result["result"] == [42, 2]
        assert result["output"] == "hello\n"
    assert effects == [[1, 2], [1, 2]]
    assert registry.manifest()[0]["name"] == "tool_length"
