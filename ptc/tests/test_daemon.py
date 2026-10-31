"""Real UDS/CLI/runner regressions and local fault injection, without worker/Hub."""

import asyncio
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import asdict

import pytest

from simplex_ptc import PTCError, Request
from simplex_ptc.client import Client
from simplex_ptc.daemon import Daemon
from simplex_ptc.endpoint import Endpoint
from simplex_ptc.lifecycle import Policy, Resources
from simplex_ptc.protocol import encode
from simplex_ptc.transport import MAX_FRAME_BYTES, execution_context, pack, receive


@pytest.fixture
def clients(tmp_path):
    """Join every daemon even if a test assertion fails."""
    created = []

    def make(name="state", modules=None, policy=None):
        client = Client(tmp_path / name, modules, policy)
        created.append(client)
        return client

    yield make
    for client in created:
        if client.endpoint.socket.exists():
            client.stop()


def context(cwd):
    return {"cwd": str(cwd), "environment": dict(os.environ)}


def eventually(check, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(0.025)
    pytest.fail("condition did not settle before deadline")


def start(client, source, cwd, identity="work", epoch=None):
    request = Request(identity, source, epoch)
    assert (
        client.call("execute", request=asdict(request), context=context(cwd))["status"] == "running"
    )
    return request


def test_one_owner_concurrent_start_and_alias(clients, tmp_path):
    first = clients()
    with ThreadPoolExecutor(max_workers=4) as pool:
        answers = list(pool.map(lambda _: clients().call("inspect"), range(4)))
    assert all(not item["exists"] for item in answers)
    generation = first.call("status")["generation"]
    alias = tmp_path / "alias"
    alias.symlink_to(first.endpoint.directory, target_is_directory=True)
    assert Client(alias).call("status")["generation"] == generation
    assert first.execute(Request("one", "x = 41\nx"))["result"] == 41
    second = clients()
    epoch = second.inspect()["epoch"]
    assert second.execute(Request("two", "x + 1", epoch))["result"] == 42
    assert second.generation == generation
    assert clients("other").inspect()["exists"] is False


def test_restart_restores_checkpoint_and_changes_generation(clients):
    client = clients()
    response = client.execute(Request("one", "x = 9\nx"))
    generation = client.generation
    client.stop()
    assert client.execute(Request("two", "x", response["epoch"]))["result"] == 9
    assert client.generation != generation


def test_status_stop_do_not_start_or_create_state(clients):
    client = clients()
    assert client.call("status", start=False) == {"running": False}
    assert client.stop() == {"running": False}
    assert not client.endpoint.directory.exists()
    assert not client.inspect()["exists"]
    assert not (client.endpoint.directory / "checkpoint").exists()


def test_runtime_and_module_mismatch_does_not_restart(clients):
    client = clients()
    client.inspect()
    generation = client.generation
    other = Client(client.endpoint.directory, ["simplex_ptc.tools.process"])
    with pytest.raises(PTCError, match="differ"):
        other.inspect()
    assert client.call("status")["generation"] == generation


def test_long_storage_path_and_stale_owned_socket(clients, tmp_path):
    client = clients("a" * 80 + "/" + "b" * 80)
    stale = socket.socket(socket.AF_UNIX)
    stale.bind(str(client.endpoint.socket))
    os.chmod(client.endpoint.socket, 0o600)
    stale.close()
    assert not client.inspect()["exists"]
    assert len(str(client.endpoint.socket)) < 104


def test_endpoint_refuses_foreign_object_and_unsafe_directory(clients):
    client = clients()
    client.endpoint.socket.write_text("keep")
    try:
        with pytest.raises(PTCError, match="socket"):
            client.inspect()
        assert client.endpoint.socket.read_text() == "keep"
    finally:
        client.endpoint.socket.unlink()
    client.endpoint.directory.mkdir(mode=0o755)
    with pytest.raises(PTCError, match="private"):
        client.inspect()


def test_cwd_environment_and_identity_are_per_call(clients, tmp_path):
    client = clients(modules=["simplex_ptc.tools.process"])
    one, two = tmp_path / "one", tmp_path / "two"
    one.mkdir()
    two.mkdir()
    ctx = context(one)
    ctx["environment"]["PTC_EXAMPLE"] = "first"
    request = Request("one", 'run_command("pwd; printf %s $PTC_EXAMPLE").stdout')
    result = client.execute(request, context=ctx)
    assert result["result"] == f"{one}\nfirst"
    assert client.execute(request, context=ctx) == result
    reordered = {"environment": dict(reversed(list(ctx["environment"].items()))), "cwd": str(one)}
    assert client.execute(request, context=reordered) == result
    ctx["environment"]["PTC_EXAMPLE"] = "second"
    with pytest.raises(PTCError) as error:
        client.execute(request, context=ctx)
    assert error.value.code == "identity_conflict"
    ctx["cwd"] = str(two)
    result = client.execute(Request("two", request.source, result["epoch"]), context=ctx)
    assert result["result"] == f"{two}\nsecond"


def test_slow_callback_busy_query_cancel_and_stop_are_responsive(clients, tmp_path):
    client = clients(modules=["simplex_ptc.tools.process"])
    started = tmp_path / "started"
    request = start(client, f'run_command("echo ready > {started}; sleep 60")', tmp_path)
    eventually(started.exists)
    before = time.monotonic()
    assert client.inspect()["active"]["execution_id"] == "work"
    assert time.monotonic() - before < 2
    with pytest.raises(PTCError) as error:
        client.call("execute", request=asdict(Request("another", "1")), context=context(tmp_path))
    assert error.value.code == "busy"
    assert (
        client.call("execute", request=asdict(request), context=context(tmp_path))["status"]
        == "running"
    )
    with pytest.raises(PTCError) as error:
        client.call(
            "execute", request=asdict(Request("work", "different")), context=context(tmp_path)
        )
    assert error.value.code == "identity_conflict"
    for operation in ("reset", "recover"):
        with pytest.raises(PTCError) as error:
            client.call(operation, epoch="bad", execution_id="work")
        assert error.value.code == "busy"
    with pytest.raises(PTCError) as error:
        client.call("cancel", generation="old", execution_id="work", epoch=None)
    assert error.value.code == "stale_execution"
    client.call("cancel", generation=client.generation, execution_id="work", epoch=None)
    result = client.wait(request)
    assert result["status"] == "failed"
    assert result["error"]["code"] in ("cancelled", "runtime_error")
    assert not result["committed"]
    assert client.stop()["stopped"]


def test_disconnected_submitter_does_not_replay_effect(clients, tmp_path):
    client = clients(modules=["simplex_ptc.tools.process"])
    target = tmp_path / "effect"
    request = Request("effect", f'run_command("echo once >> {target}; sleep 0.2").exit_code')
    sock = client.ensure()
    sock.sendall(
        pack(
            {
                "id": "lost-reply",
                "operation": "execute",
                "request": asdict(request),
                "context": context(tmp_path),
            }
        )
    )
    sock.close()
    eventually(target.exists)
    assert client.wait(request)["committed"]
    assert client.execute(request, context=context(tmp_path))["committed"]
    assert target.read_text() == "once\n"
    with pytest.raises(PTCError) as error:
        client.call("query", execution_id="unknown", epoch=None)
    assert error.value.code == "unknown_execution"


def test_stop_active_execution_releases_owned_process(clients, tmp_path):
    client = clients(modules=["simplex_ptc.tools.process"])
    pid_file = tmp_path / "pid"
    start(client, f'run_command("echo $$ > {pid_file}; exec sleep 60")', tmp_path)
    eventually(pid_file.exists)
    pid = int(pid_file.read_text())
    assert client.stop()["stopped"]
    with pytest.raises(ProcessLookupError):
        os.kill(pid, 0)


def test_detached_process_is_not_newly_owned_by_daemon(clients, tmp_path):
    client = clients(modules=["simplex_ptc.tools.process"])
    pid_file = tmp_path / "pid"
    source = f'run_command("echo $$ > {pid_file}; exec sleep 60 >/dev/null 2>&1", timeout_seconds=0.1, timeout_action="detach").to_dict()'
    try:
        result = client.execute(Request("detach", source))
        assert result["result"]["status"] == "detached"
        pid = int(pid_file.read_text())
        client.stop()
        os.kill(pid, 0)
    finally:
        if pid_file.exists():
            try:
                os.kill(int(pid_file.read_text()), signal.SIGKILL)
            except ProcessLookupError:
                pass


def test_idle_expiry_ignores_passive_polling(clients):
    policy = Policy(idle_seconds=0.3, resource_seconds=1, cleanup_seconds=1)
    client = clients(policy=policy)
    client.inspect()
    generation = client.generation
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline and client.endpoint.socket.exists():
        try:
            client.call("status", start=False)
        except PTCError:
            pass
        time.sleep(0.03)
    assert not client.endpoint.socket.exists()
    client.inspect()
    assert client.generation != generation


def test_incomplete_frame_and_oversize_do_not_block_other_clients(clients):
    client = clients()
    client.inspect()
    incomplete = client.endpoint.connect()
    oversized = client.endpoint.connect()
    try:
        incomplete.sendall(b"\x00")
        oversized.sendall(struct.pack("!I", MAX_FRAME_BYTES + 1))
        assert receive(oversized)["boundary_error"]["error"]["code"] == "transport_limit"
        assert client.call("status")["state"] == "ready"
        assert client.stop()["stopped"]
    finally:
        incomplete.close()
        oversized.close()


def test_daemon_death_leaves_pending_and_runner_cleans_up(clients, tmp_path):
    client = clients(modules=["simplex_ptc.tools.process"])
    pid_file = tmp_path / "pid"
    start(client, f'run_command("echo $$ > {pid_file}; exec sleep 60")', tmp_path)
    eventually(pid_file.exists)
    pid = int(pid_file.read_text())
    sock = client.endpoint.connect()
    _, uid, _ = struct.unpack("3i", sock.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
    daemon_pid, _, _ = struct.unpack(
        "3i", sock.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
    )
    assert uid == os.getuid()
    sock.close()
    os.kill(daemon_pid, signal.SIGKILL)

    def gone():
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return True
        return False

    eventually(gone)
    info = client.inspect()
    assert info["recovery_required"]
    with pytest.raises(PTCError) as error:
        client.execute(Request("new", "1", info["epoch"]))
    assert error.value.code == "recovery_required"
    client.recover(info["epoch"], "work")
    assert client.execute(Request("new", "1", info["epoch"]))["result"] == 1


def test_resource_generation_retention_and_all_cleanup():
    async def scenario():
        now = [0]
        registry = Resources("generation", Policy(1, 5, 0.05), lambda: now[0])
        closed = []

        class Resource:
            def __init__(self, name):
                self.name = name

            def alive(self):
                return True

            async def close(self):
                closed.append(self.name)
                if self.name == "fail":
                    raise OSError("cleanup failed")
                await asyncio.sleep(60)

        key = registry.add(Resource("fail"))
        registry.add(Resource("slow"))
        now[0] = 2
        assert not registry.expired(False)
        now[0] = 6
        assert registry.expired(False)
        assert not registry.expired(True)
        registry.get(key)
        assert not registry.expired(False)
        with pytest.raises(PTCError) as error:
            Resources("new", Policy()).get(key)
        assert error.value.code == "resource_lost"
        with pytest.raises(OSError):
            await registry.close()
        assert closed == ["fail", "slow"]
        assert not registry.items
        with pytest.raises(PTCError) as error:
            registry.add(Resource("late"))
        assert error.value.code == "stopping"

    asyncio.run(scenario())


@pytest.mark.parametrize("value", [0, -1, float("inf"), float("nan"), True])
def test_policy_rejects_invalid_deadlines(value):
    with pytest.raises(ValueError):
        Policy(idle_seconds=value)


def test_execution_context_rejects_invalid_environment(tmp_path):
    for environment in ({"bad=name": "value"}, {"x": "nul\0"}, {"x": 1}):
        with pytest.raises(PTCError):
            execution_context({"cwd": str(tmp_path), "environment": environment})


def test_publication_failure_remains_pending_without_replay(tmp_path, monkeypatch):
    async def scenario():
        daemon = Daemon(Endpoint(tmp_path / "state"), [], Policy())
        original = daemon.session.storage.clear_pending

        def fail():
            raise OSError("sync failure")

        monkeypatch.setattr(daemon.session.storage, "clear_pending", fail)
        request = Request("one", "x = 1\nx")
        value = {"operation": "execute", "request": asdict(request), "context": context(tmp_path)}
        daemon.admit(value)
        await daemon.task
        assert daemon.last[2]["error"]["code"] == "storage_or_interruption"
        assert (tmp_path / "state" / "execution.pending").exists()
        monkeypatch.setattr(daemon.session.storage, "clear_pending", original)
        daemon.admit(value)
        await daemon.task
        assert daemon.last[2]["revision"] == 1
        assert daemon.last[2]["result"] == 1
        assert not (tmp_path / "state" / "execution.pending").exists()
        await daemon.shutdown()

    asyncio.run(scenario())


def test_low_level_session_cannot_bypass_daemon_owner(clients):
    from simplex_ptc import Session

    client = clients()
    client.inspect()
    with pytest.raises(PTCError) as error:
        Session(client.endpoint.directory).execute(Request("bypass", "1"))
    assert error.value.code == "daemon_busy"


def test_lifetime_lock_not_replaced_or_inherited_by_commands(clients, tmp_path):
    client = clients(modules=["simplex_ptc.tools.process"])
    lock = client.endpoint.directory / "daemon.lock"
    client.inspect()
    inode = lock.stat().st_ino
    output = client.execute(Request("fd", 'run_command("ls -l /proc/self/fd").stdout'))
    assert "daemon.lock" not in output["result"]
    assert ".sock" not in output["result"]
    client.stop()
    client.inspect()
    assert lock.stat().st_ino == inode


def test_cli_sigterm_requests_cancel_and_preserves_failure_exit_code(clients, tmp_path):
    client = clients(modules=["simplex_ptc.tools.process"])
    marker = tmp_path / "running"
    process = subprocess.Popen(
        [
            sys.executable,
            "-m",
            "simplex_ptc",
            "--module",
            "simplex_ptc.tools.process",
            "execute",
            "--state-directory",
            str(client.endpoint.directory),
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        process.stdin.write(
            encode(
                {
                    "execution_id": "signal",
                    "source": f'run_command("echo ready > {marker}; sleep 60")',
                }
            )
        )
        process.stdin.close()
        process.stdin = None
        eventually(marker.exists)
        process.send_signal(signal.SIGTERM)
        stdout, stderr = process.communicate(timeout=15)
        assert process.returncode == 1, stderr
        assert json.loads(stdout)["status"] == "failed"
        assert client.call("status", start=False)["active"] is None
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()


@pytest.fixture
def fault_module(tmp_path, monkeypatch):
    """Trusted test callbacks force failure outside Monty's normal execution limits."""
    name = "ptc_daemon_fault_fixture"
    (tmp_path / f"{name}.py").write_text("""
import os
import signal
import time
from simplex_ptc import ToolDescriptor

def crash():
    os._exit(77)

def hang(path):
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    with open(path, "w") as out:
        out.write(str(os.getpid()))
    time.sleep(60)

def register_tools(registry):
    registry.add(ToolDescriptor("crash", "Crash", "crash()", "none", "exit", "1"), crash)
    registry.add(ToolDescriptor("hang", "Hang", "hang(path)", "none", "write", "1"), hang)
""")
    monkeypatch.syspath_prepend(str(tmp_path))
    monkeypatch.setenv("PYTHONPATH", str(tmp_path))
    yield name
    sys.modules.pop(name, None)


def test_runner_crash_requires_explicit_recovery(clients, fault_module, tmp_path):
    client = clients(modules=[fault_module])
    with pytest.raises(PTCError) as error:
        client.execute(Request("crash", "crash()"))
    assert error.value.code == "storage_or_interruption"
    info = client.inspect()
    assert info["recovery_required"]
    client.recover(info["epoch"], "crash")
    assert client.execute(Request("after", "1", info["epoch"]))["committed"]


def test_stuck_runner_forced_cancel_reports_unknown_effects(clients, fault_module, tmp_path):
    client = clients(modules=[fault_module], policy=Policy(cleanup_seconds=0.3))
    marker = tmp_path / "pid"
    request = start(client, f"hang({str(marker)!r})", tmp_path)
    eventually(marker.exists)
    pid = int(marker.read_text())
    before = time.monotonic()
    client.call("cancel", generation=client.generation, execution_id="work", epoch=None)
    with pytest.raises(PTCError) as error:
        client.wait(request)
    assert error.value.code == "storage_or_interruption"
    assert time.monotonic() - before < 3
    assert client.inspect()["recovery_required"]
    with pytest.raises(ProcessLookupError):
        os.kill(pid, 0)


def test_file_edit_semantics_survive_runner_boundary(clients, tmp_path):
    client = clients(modules=["simplex_ptc.tools.files"])
    target = tmp_path / "document"
    target.write_text("before\n")
    source = f'f = open_text({str(target)!r}, writable=True)\nf.replace("before", "after")\nf.preview().to_dict()'
    preview = client.execute(Request("stage", source))
    assert preview["committed"]
    assert "after" in preview["result"]["text"]
    assert target.read_text() == "before\n"
    completed = client.execute(
        Request("commit", source + "\nf.commit().to_dict()", preview["epoch"])
    )
    assert completed["result"]["changed"]
    assert target.read_text() == "after\n"


def test_drain_closes_admission_before_cleanup(tmp_path):
    async def scenario():
        daemon = Daemon(Endpoint(tmp_path / "state"), [], Policy())
        daemon.begin_shutdown()
        with pytest.raises(PTCError) as error:
            daemon.admit({"request": asdict(Request("late", "1")), "context": context(tmp_path)})
        assert error.value.code == "stopping"
        assert (await daemon.shutdown_task)["stopped"]
        assert not (tmp_path / "state" / "checkpoint").exists()

    asyncio.run(scenario())


def test_management_can_stop_after_plugin_change_but_cannot_execute(clients):
    client = clients(modules=["simplex_ptc.tools.process"])
    client.inspect()
    manager = Client(client.endpoint.directory, ["no_such_installed_module"])
    assert manager.call("status", start=False)["running"]
    with manager.connect(management=True) as sock:
        sock.sendall(pack({"id": "forbidden", "operation": "reset", "epoch": None}))
        reply = receive(sock)
        assert reply["result"]["boundary_error"]["error"]["code"] == "invalid_operation"
    assert manager.stop()["stopped"]


def test_lifetime_lock_symlink_rejected_without_modifying_target(tmp_path):
    endpoint = Endpoint(tmp_path / "state")
    endpoint.directory.mkdir(mode=0o700)
    target = tmp_path / "target"
    target.write_text("keep")
    (endpoint.directory / "daemon.lock").symlink_to(target)
    with pytest.raises(OSError):
        with endpoint.locked():
            pytest.fail("followed unsafe lifetime lock")
    assert target.read_text() == "keep"


def test_boundary_error_cli_keeps_exit_two(clients):
    client = clients()
    result = client.execute(Request("one", "1"))
    process = subprocess.run(
        [
            sys.executable,
            "-m",
            "simplex_ptc",
            "execute",
            "--state-directory",
            str(client.endpoint.directory),
        ],
        input=json.dumps({"execution_id": "bad", "source": "1", "epoch": "0" * 36}),
        text=True,
        capture_output=True,
        timeout=10,
    )
    assert process.returncode == 2
    assert json.loads(process.stdout)["error"]["code"] == "stale_epoch"
    assert client.inspect()["revision"] == result["revision"]
