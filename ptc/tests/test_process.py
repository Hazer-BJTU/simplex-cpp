"""Managed command behavior: real pipes/processes and real cross-invocation Monty handles."""

import asyncio
import fcntl
import os
import shlex
import sys
import time
from contextlib import asynccontextmanager

import pytest

from simplex_ptc import Client, PTCError, Request
from simplex_ptc import commands as impl
from simplex_ptc.lifecycle import Policy, Resources
from simplex_ptc.transport import caller_umask


def python_command(source):
    return f"exec {shlex.quote(sys.executable)} -c {shlex.quote(source)}"


def context(tmp_path):
    return dict(cwd=str(tmp_path), environment=dict(os.environ), umask=caller_umask())


@asynccontextmanager
async def manager(tmp_path):
    resources = Resources("test-generation", Policy())
    commands = impl.Commands(resources)
    created = []

    async def call(operation, **arguments):
        return await commands.dispatch(
            dict(arguments, operation=operation), context(tmp_path), created
        )

    try:
        yield commands, call, created
    finally:
        await commands.close()


async def launch(call, source, **kwargs):
    return (
        await call(
            "launch",
            command=source,
            working_directory=None,
            timeout_seconds=kwargs.get("timeout_seconds", 2),
            timeout_action=kwargs.get("timeout_action", "keep"),
        )
    )["id"]


def test_idle_capture_waits_for_readiness_and_removes_readers(tmp_path, monkeypatch):
    async def scenario():
        loop = asyncio.get_running_loop()
        registered = asyncio.Event()
        output = asyncio.Event()
        readers = set()
        callbacks = 0
        add_reader, remove_reader = loop.add_reader, loop.remove_reader

        def add(fd, callback, *args):
            def ready():
                nonlocal callbacks
                callbacks += 1
                callback(*args)
                output.set()

            add_reader(fd, ready)
            readers.add(fd)
            if len(readers) == 2:
                registered.set()

        def remove(fd):
            readers.discard(fd)
            return remove_reader(fd)

        monkeypatch.setattr(loop, "add_reader", add)
        monkeypatch.setattr(loop, "remove_reader", remove)
        async with manager(tmp_path) as (store, call, _):
            identity = await launch(call, "exec cat", timeout_seconds=0)
            item = store.records[identity]
            await asyncio.wait_for(registered.wait(), 2)
            assert len(item.readers) == 2

            async def no_polling(*args):
                pytest.fail("idle capture must not schedule periodic sleeps")

            with monkeypatch.context() as patch:
                patch.setattr(impl.asyncio, "sleep", no_polling)
                # Explicit event-loop checkpoints, without a wall-clock sleep.
                for _ in range(5):
                    checkpoint = loop.create_future()
                    loop.call_soon(checkpoint.set_result, None)
                    await checkpoint
                assert callbacks == 0 and not item.pump.done()
                await call(
                    "send", id=identity, text="later\n", close_stdin=False, timeout_seconds=0
                )
                await asyncio.wait_for(output.wait(), 2)
                assert (await call("read", id=identity, full=False, max_bytes=100))[
                    "stdout"
                ] == "later\n"
            await call("send", id=identity, text="", close_stdin=True, timeout_seconds=0)
            await asyncio.wait_for(asyncio.shield(item.pump), 2)
            assert item.process.returncode == 0 and item.complete()
            assert not readers and not item.readers
            assert (await call("release", id=identity))["released"]

    asyncio.run(scenario())


def test_pipe_eof_before_process_exit_still_reaps_child(tmp_path):
    async def scenario():
        async with manager(tmp_path) as (store, call, _):
            identity = await launch(call, "exec 1>&- 2>&-; read line", timeout_seconds=0)
            item = store.records[identity]
            await asyncio.wait_for(item.streams_done.wait(), 2)
            assert item.complete() and item.process.poll() is None
            await call("send", id=identity, text="done\n", close_stdin=True, timeout_seconds=0)
            await asyncio.wait_for(asyncio.shield(item.pump), 2)
            assert item.process.returncode == 0 and item.process.stdin.closed

    asyncio.run(scenario())


def test_separate_output_nonzero_exit_and_release(tmp_path):
    async def scenario():
        async with manager(tmp_path) as (_, call, created):
            identity = await launch(call, "printf hello; printf warning >&2; exit 7")
            result = await call("read", id=identity, full=False, max_bytes=16384)
            assert (result["stdout"], result["stderr"], result["exit_code"]) == (
                "hello",
                "warning",
                7,
            )
            assert result["output_complete"] and not result["output_truncated"]
            assert created == [identity]
            assert (await call("release", id=identity))["released"]
            with pytest.raises(PTCError) as error:
                await call("get", id=identity)
            assert error.value.code == "resource_lost"

    asyncio.run(scenario())


def test_live_stdin_eof_wait_and_shared_cursors(tmp_path):
    async def scenario():
        async with manager(tmp_path) as (_, call, _):
            identity = await launch(call, "cat", timeout_seconds=0)
            assert (await call("status", id=identity))["status"] == "running"
            assert not (await call("release", id=identity))["released"]
            sent = await call(
                "send", id=identity, text="hello\n", close_stdin=True, timeout_seconds=1
            )
            assert sent["input_complete"] and sent["input_bytes_written"] == 6
            assert not (await call("wait", id=identity, timeout_seconds=2))["timed_out"]
            assert (await call("read", id=identity, full=True, max_bytes=4))["stdout"] == "hell"
            first = await call("read", id=identity, full=False, max_bytes=4)
            assert first["stdout"] == "hell" and first["stdout_cursor"] == 4
            await call("get", id=identity)
            second = await call("read", id=identity, full=False, max_bytes=4)
            assert second["stdout"] == "o\n" and second["stdout_cursor"] == 6
            assert (await call("read", id=identity, full=False, max_bytes=4))["stdout"] == ""
            assert (await call("read", id=identity, full=True, max_bytes=10))["stdout"] == "hello\n"

    asyncio.run(scenario())


def test_shared_prefix_budget_continues_draining_both_streams(tmp_path):
    async def scenario():
        async with manager(tmp_path) as (store, call, _):
            source = python_command(
                "import os\nfor i in range(100):\n os.write(1,b'a'*4096)\n os.write(2,b'b'*4096)"
            )
            identity = await launch(call, source, timeout_seconds=10)
            item = store.records[identity]
            assert sum(map(len, item.data.values())) == impl.CAPTURE_BYTES
            assert sum(item.omitted.values()) == 819200 - impl.CAPTURE_BYTES
            result = await call("read", id=identity, full=False, max_bytes=65536)
            assert result["exit_code"] == 0 and result["output_complete"]
            assert result["output_truncated"]
            assert any("redirect" in hint for hint in result["hints"])
            assert item.remaining == 0
            assert (await call("read", id=identity, full=True, max_bytes=65536))[
                "stdout"
            ] == result["stdout"]

    asyncio.run(scenario())


def test_utf8_split_is_deferred_and_invalid_bytes_are_replaced(tmp_path):
    async def scenario():
        async with manager(tmp_path) as (store, call, _):
            identity = await launch(call, "sleep 60", timeout_seconds=0)
            item = store.records[identity]
            # Deterministic byte boundaries exercise the same reader used by the pump.
            item.data["stdout"].extend(b"\xe4\xb8")
            first = await call("read", id=identity, full=False, max_bytes=100)
            assert first["stdout"] == "" and first["stdout_cursor"] == 0
            item.data["stdout"].extend(b"\xad\xff")
            second = await call("read", id=identity, full=False, max_bytes=100)
            assert second["stdout"] == "中�" and second["stdout_cursor"] == 4
            item.data["stdout"].extend(b"\xe4")
            item.omitted["stdout"] = 1
            assert (await call("read", id=identity, full=False, max_bytes=100))["stdout"] == "�"

    asyncio.run(scenario())


def test_kill_timeout_and_explicit_stop_reap_group(tmp_path):
    async def scenario():
        async with manager(tmp_path) as (_, call, _):
            target = tmp_path / "unexpected"
            identity = await launch(
                call,
                f"(sleep 0.3; touch {target}) & wait",
                timeout_seconds=0.02,
                timeout_action="kill",
            )
            result = await call("status", id=identity)
            assert result["status"] == "killed" and result["timed_out"]
            with pytest.raises(ProcessLookupError):
                os.kill(result["pid"], 0)
            await asyncio.sleep(0.4)
            assert not target.exists()
            second = await launch(call, "sleep 60", timeout_seconds=0)
            assert (await call("stop", id=second))["status"] == "killed"
            assert (await call("stop", id=second))["status"] == "killed"

    asyncio.run(scenario())


def test_descendant_held_pipes_distinguish_exit_from_capture(tmp_path):
    async def scenario():
        async with manager(tmp_path) as (_, call, _):
            identity = await launch(call, "sleep 0.8 & exit 0", timeout_seconds=0.05)
            status = await call("status", id=identity)
            assert status["status"] == "exited" and not status["output_complete"]
            result = await call("release", id=identity)
            assert result["released"] and result["output_complete"] and result["output_truncated"]
            assert result["stdout_end"] == "stopped"

    asyncio.run(scenario())


@pytest.mark.skipif(not hasattr(fcntl, "F_SETPIPE_SZ"), reason="Linux pipe-capacity control")
def test_partial_stdin_does_not_close_or_implicitly_resend(tmp_path):
    async def scenario():
        async with manager(tmp_path) as (store, call, _):
            identity = await launch(call, "sleep 60", timeout_seconds=0)
            fd = store.records[identity].process.stdin.fileno()
            capacity = fcntl.fcntl(fd, fcntl.F_SETPIPE_SZ, 4096)
            for _ in range(0, capacity - 4096, 4096):
                assert os.write(fd, b"p" * 4096) == 4096
            result = await call(
                "send", id=identity, text="x" * 65536, close_stdin=True, timeout_seconds=0
            )
            assert not result["input_complete"]
            assert 0 < result["input_bytes_written"] < 65536
            assert not store.records[identity].process.stdin.closed

    asyncio.run(scenario())


@pytest.mark.parametrize(
    "arguments",
    [
        {"command": ""},
        {"command": "bad\0"},
        {"timeout_action": "detach"},
        {"timeout_seconds": -1},
        {"timeout_seconds": True},
        {"timeout_seconds": float("nan")},
        {"timeout_seconds": float("inf")},
        {"timeout_seconds": 301},
        {"working_directory": ""},
        {"timeout_seconds": 0, "timeout_action": "kill"},
    ],
)
def test_invalid_launch_has_no_effect(tmp_path, arguments):
    async def scenario():
        async with manager(tmp_path) as (store, call, created):
            values = dict(
                command="true", working_directory=None, timeout_seconds=1, timeout_action="keep"
            )
            values.update(arguments)
            with pytest.raises((ValueError, PTCError)):
                await call("launch", **values)
            assert not store.records and not created

    asyncio.run(scenario())


def test_capacity_and_constructor_failure_cleanup(tmp_path, monkeypatch):
    async def scenario():
        async with manager(tmp_path) as (store, call, created):
            monkeypatch.setattr(impl, "MAX_RECORDS", 1)
            identity = await launch(call, "true")
            with pytest.raises(PTCError) as error:
                await launch(call, "true")
            assert error.value.code == "command_limit"
            await call("release", id=identity)
            created.clear()  # next invocation has its own bounded effects list

            def fail(process_id, process):
                nonlocal captured
                captured = process
                raise OSError("capture setup failed")

            captured = None
            monkeypatch.setattr(impl, "ManagedCommand", fail)
            with pytest.raises(OSError):
                await launch(call, "sleep 60")
            assert captured.returncode is not None
            assert captured.stdout.closed and captured.stderr.closed and captured.stdin.closed
            assert not store.records

    asyncio.run(scenario())


@pytest.fixture
def client(tmp_path):
    value = Client(tmp_path / "state", ["simplex_ptc.tools.process"])
    yield value
    value.stop()


def execute(client, identity, source):
    return client.execute(Request(identity, source, client.inspect()["epoch"]))


def test_cross_invocation_handle_stdin_and_output(client):
    first = execute(
        client, "one", 'c = run_command("cat", timeout_seconds=0)\ncommand_id = c.id\ncommand_id'
    )
    identity = first["result"]
    assert identity in first["commands_created"]
    second = execute(
        client,
        "two",
        'c = get_command(command_id)\nc.send("hello\\n", close_stdin=True)\nc.wait(timeout_seconds=2)\nc.read().to_dict()',
    )
    assert second["result"]["stdout"] == "hello\n"
    assert second["result"]["output_complete"]
    assert execute(client, "three", "get_command(command_id).read().stdout")["result"] == ""
    assert (
        execute(client, "four", "get_command(command_id).read(full=True).stdout")["result"]
        == "hello\n"
    )
    assert execute(client, "five", "get_command(command_id).release().released")["result"]
    assert (
        execute(client, "six", "get_command(command_id).status().to_dict()")["status"] == "failed"
    )


def test_failed_program_lists_created_command_and_retains_it(client):
    failed = execute(
        client, "failed", 'c = run_command("sleep 60", timeout_seconds=0)\nraise ValueError("fail")'
    )
    assert failed["status"] == "failed" and not failed["committed"]
    identity = failed["commands_created"][0]
    status = execute(client, "query", f"get_command({identity!r}).status().to_dict()")
    assert status["result"]["status"] == "running"
    stopped = execute(client, "stop", f"get_command({identity!r}).stop().to_dict()")
    assert stopped["result"]["status"] == "killed"


def test_handles_are_not_serializable_and_generation_is_not_restored(client):
    failed = execute(client, "handle", 'run_command("true")')
    assert failed["error"]["code"] == "conversion_error"
    identity = failed["commands_created"][0]
    assert execute(client, "check", f"get_command({identity!r}).status().exit_code")["result"] == 0
    client.stop()
    assert execute(client, "lost", f"get_command({identity!r})")["status"] == "failed"


def test_retry_does_not_launch_again(client, tmp_path):
    marker = tmp_path / "effect"
    request = Request("once", f'run_command("echo once >> {marker}").id')
    first = client.execute(request)
    assert client.execute(request) == first
    assert marker.read_text() == "once\n"


def test_cancel_initial_wait_keeps_command_queryable(client, tmp_path):
    marker = tmp_path / "ready"
    request = Request("waiting", f'run_command("echo ready > {marker}; sleep 60").id')
    ctx = context(tmp_path)
    client.call(
        "execute",
        request={"execution_id": request.execution_id, "source": request.source},
        context=ctx,
    )
    deadline = time.monotonic() + 5
    while not marker.exists() and time.monotonic() < deadline:
        time.sleep(0.02)
    assert marker.exists()
    query = client.call("query", execution_id="waiting", epoch=None)
    identity = query["commands_created"][0]
    client.call("cancel", generation=client.generation, execution_id="waiting", epoch=None)
    result = client.wait(request)
    assert result["status"] == "failed" and result["commands_created"] == [identity]
    assert (
        execute(client, "observe", f"get_command({identity!r}).status().status")["result"]
        == "running"
    )


def test_retention_expiry_stops_live_process(tmp_path):
    client = Client(tmp_path / "state", ["simplex_ptc.tools.process"], Policy(0.2, 0.4, 10))
    try:
        response = execute(
            client, "one", 'run_command("exec sleep 60", timeout_seconds=0).status().to_dict()'
        )
        pid = response["result"]["pid"]
        deadline = time.monotonic() + 4
        while client.endpoint.socket.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert not client.endpoint.socket.exists()
        with pytest.raises(ProcessLookupError):
            os.kill(pid, 0)
    finally:
        client.stop()


def test_shutdown_attempts_every_command_after_cleanup_error(tmp_path, monkeypatch):
    async def scenario():
        async with manager(tmp_path) as (store, call, _):
            one = await launch(call, "exec sleep 60", timeout_seconds=0)
            two = await launch(call, "exec sleep 60", timeout_seconds=0)
            processes = [store.records[key].process for key in (one, two)]
            real_kill = os.killpg

            def fail_group(pid, sig):
                if pid == processes[0].pid:
                    raise OSError("group failure")
                real_kill(pid, sig)

            monkeypatch.setattr(os, "killpg", fail_group)
            with pytest.raises(OSError, match="group failure"):
                await store.close()
            assert all(p.poll() is not None for p in processes)
            assert all(p.stdin.closed and p.stdout.closed and p.stderr.closed for p in processes)
            assert not store.records

    asyncio.run(scenario())


def test_initial_kill_wait_cancellation_cleans_child(client, tmp_path):
    marker = tmp_path / "ready"
    request = Request(
        "waiting", f'run_command("echo ready > {marker}; exec sleep 60", timeout_action="kill").id'
    )
    client.call(
        "execute",
        request={"execution_id": "waiting", "source": request.source},
        context=context(tmp_path),
    )
    deadline = time.monotonic() + 5
    while not marker.exists() and time.monotonic() < deadline:
        time.sleep(0.02)
    assert marker.exists()
    client.call("cancel", generation=client.generation, execution_id="waiting", epoch=None)
    result = client.wait(request)
    assert result["status"] == "failed"
    identity = result["commands_created"][0]
    observed = execute(client, "query", f"get_command({identity!r}).status().to_dict()")
    assert observed["result"]["status"] == "killed"


def test_result_budget_does_not_lose_created_ids(client):
    result = execute(client, "large", 'c = run_command("true")\n"x" * 100000')
    assert result["committed"] and result["result_truncated"]
    assert len(result["commands_created"]) == 1
    identity = result["commands_created"][0]
    assert execute(client, "read", f"get_command({identity!r}).status().exit_code")["result"] == 0


def test_two_sessions_cannot_get_each_others_commands(client, tmp_path):
    first = execute(client, "one", 'run_command("true").id')
    other = Client(tmp_path / "other", ["simplex_ptc.tools.process"])
    try:
        response = execute(other, "foreign", f"get_command({first['result']!r})")
        assert response["status"] == "failed"
    finally:
        other.stop()


def test_private_rpc_channel_not_inherited_by_shell(client):
    result = execute(client, "fds", 'run_command("ls -l /proc/self/fd").read().stdout')
    assert "socket:" not in result["result"]
    assert "daemon.lock" not in result["result"]


def test_read_ipc_budget_and_full_observation_preserve_cursor(tmp_path):
    async def scenario():
        async with manager(tmp_path) as (_, call, _):
            identity = await launch(call, python_command("import os; os.write(1,b'\\x00'*65536)"))
            full = await call("read", id=identity, full=True, max_bytes=65536)
            assert full["stdout_cursor"] == 0 and not full["read_limited"]
            assert len(full["stdout"]) == 65536
            first = await call("read", id=identity, full=False, max_bytes=65536)
            assert first["stdout"] == full["stdout"]
            assert first["stdout_cursor"] == len(first["stdout"])

    asyncio.run(scenario())


@pytest.mark.skipif(not hasattr(fcntl, "F_SETPIPE_SZ"), reason="Linux pipe-capacity control")
@pytest.mark.parametrize("seconds", [0, 0.03])
@pytest.mark.parametrize("text", ["中" * 20000, "🙂" * 15000, "a中🙂é" * 5000])
def test_backpressured_utf8_send_can_resume_exactly_through_monty(client, tmp_path, seconds, text):
    ready = tmp_path / "ready"
    drain = tmp_path / "drain"
    received = tmp_path / "received"
    child = f"""import fcntl, sys, time
from pathlib import Path
capacity = fcntl.fcntl(0, fcntl.F_SETPIPE_SZ, 4096)
marker = Path({str(ready)!r})
marker.with_suffix(".tmp").write_text(str(capacity))
marker.with_suffix(".tmp").replace(marker)
while not Path({str(drain)!r}).exists():
    time.sleep(0.005)
Path({str(received)!r}).write_bytes(sys.stdin.buffer.read())
"""
    source = f"c = run_command({python_command(child)!r}, timeout_seconds=0)\ncommand_id = c.id\ncommand_id"
    launched = execute(client, "launch", source)
    assert launched["committed"]
    deadline = time.monotonic() + 5
    while not ready.exists() and time.monotonic() < deadline:
        time.sleep(0.01)
    assert ready.exists()
    # Some Linux architectures round capacity up to a page larger than 4096.
    # Leave exactly one atomic-write-sized window rather than assume pipe size.
    capacity = int(ready.read_text())
    padding = b"x" * max(0, capacity - 4096)
    for offset in range(0, len(padding), 65536):
        prefix = padding[offset : offset + 65536].decode("ascii")
        filled = execute(
            client,
            f"fill-{offset}",
            f"get_command(command_id).send({prefix!r}, timeout_seconds=1).to_dict()",
        )
        assert filled["result"]["input_complete"]
    partial = execute(
        client,
        "partial",
        f"get_command(command_id).send({text!r}, close_stdin=True, timeout_seconds={seconds}).to_dict()",
    )
    assert partial["committed"], partial
    result = partial["result"]
    raw = text.encode("utf-8")
    written = result["input_bytes_written"]
    assert not result["input_complete"]
    assert 0 < written < len(raw)
    # Both halves must be legal text, and the byte count must describe the real
    # input prefix rather than hiding already-written continuation bytes.
    prefix = raw[:written].decode("utf-8")
    suffix = raw[written:].decode("utf-8")
    assert prefix + suffix == text
    drain.touch()
    resumed = execute(
        client,
        "resume",
        f"get_command(command_id).send({suffix!r}, close_stdin=True, timeout_seconds=5).to_dict()",
    )
    assert resumed["committed"], resumed
    assert resumed["result"]["input_complete"]
    assert resumed["result"]["input_bytes_written"] == len(raw) - written
    settled = execute(client, "settle", "get_command(command_id).wait(timeout_seconds=5).to_dict()")
    assert settled["result"]["exit_code"] == 0
    assert received.read_bytes() == padding + raw


def test_atomic_write_contract_violation_reports_actual_effect_count(tmp_path, monkeypatch):
    async def scenario():
        async with manager(tmp_path) as (store, call, _):
            identity = await launch(call, "sleep 60", timeout_seconds=0)
            fd = store.records[identity].process.stdin.fileno()
            write = os.write

            def partial(target, data):
                return write(target, data[:1] if target == fd else data)

            monkeypatch.setattr(os, "write", partial)
            with pytest.raises(OSError, match="after 1 bytes"):
                await call("send", id=identity, text="中", close_stdin=True, timeout_seconds=0)
            assert not store.records[identity].process.stdin.closed

    asyncio.run(scenario())
