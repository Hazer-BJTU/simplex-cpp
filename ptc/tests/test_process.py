"""Package-local shell tests; all spawned work is bounded or explicitly cleaned."""

import json
import os
import shlex
import signal
import subprocess
import sys
import time

import pytest

from simplex_ptc import Request, Session, ToolRegistry
from simplex_ptc.tools import process as impl


def command(source, **kwargs):
    return impl.run_command(source, **kwargs).value


def python_command(source):
    return f"exec {shlex.quote(sys.executable)} -c {shlex.quote(source)}"


def reap_test_child(pid):
    """Test cleanup for a detached process; no production task-management API."""
    try:
        os.killpg(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    # The production daemon waiter owns waitpid. Avoid racing it here.
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return
        time.sleep(0.01)
    pytest.fail("detached child was not reaped")


def test_shell_stdout_stderr_exit_and_stdin_eof(tmp_path):
    result = command(
        "read input || printf eof; printf warning >&2; exit 7", working_directory=str(tmp_path)
    )
    assert result.status == "exited"
    assert result.exit_code == 7
    assert result.stdout == "eof"
    assert result.stderr == "warning"
    assert result.output_complete
    assert not result.timed_out
    assert result.termination_signal is None
    assert result.to_dict()["hints"] == []
    assert "[stdout]" in result.render() and "[stderr]" in result.render()


def test_cwd_environment_and_shell_redirection(tmp_path, monkeypatch):
    monkeypatch.setenv("PTC_TEST_VALUE", "inherited")
    result = command(
        'printf "%s" "$PTC_TEST_VALUE" > output; cat output; pwd', working_directory=str(tmp_path)
    )
    assert result.stdout == f"inherited{tmp_path}\n"
    assert (tmp_path / "output").read_text() == "inherited"


def test_signal_exit_is_separate_from_exit_code():
    result = command("kill -TERM $$")
    assert result.termination_signal == signal.SIGTERM
    assert result.exit_code is None
    assert result.status == "exited"


def test_large_dual_stream_output_is_drained_but_capture_is_bounded():
    result = command(
        python_command(
            "import os\nfor _ in range(100):\n os.write(1, b'a'*4096)\n os.write(2, b'b'*4096)"
        ),
        timeout_seconds=10,
    )
    assert result.exit_code == 0
    assert result.output_complete
    assert len(result.stdout) + len(result.stderr) == impl.CAPTURE_BYTES
    assert result.stdout_omitted_bytes + result.stderr_omitted_bytes == 819200 - impl.CAPTURE_BYTES
    assert any("redirect" in hint for hint in result.hints)


def test_invalid_utf8_is_replaced():
    result = command(python_command("import os; os.write(1, b'\\xff')"))
    assert result.stdout == "\ufffd"
    assert any("UTF-8" in hint for hint in result.hints)


def test_kill_timeout_reaps_direct_child():
    result = command("exec sleep 10", timeout_seconds=0.05)
    assert result.status == "killed"
    assert result.timed_out
    assert result.termination_signal == signal.SIGKILL
    with pytest.raises(ProcessLookupError):
        os.kill(result.pid, 0)


def test_kill_stops_group_side_effect(tmp_path):
    target = tmp_path / "should-not-exist"
    result = command(f"(sleep 0.4; touch {shlex.quote(str(target))}) & wait", timeout_seconds=0.05)
    assert result.timed_out
    time.sleep(0.5)
    assert not target.exists()


def test_detach_returns_without_waiting_and_does_not_bind_lifetime():
    start = time.monotonic()
    result = command("exec sleep 10", timeout_seconds=0, timeout_action="detach")
    try:
        assert time.monotonic() - start < 2
        assert result.status == "detached"
        assert not result.timed_out
        assert result.exit_code is None
        assert any("SIGPIPE" in hint for hint in result.hints)
        os.kill(result.pid, 0)
    finally:
        reap_test_child(result.pid)


def test_detach_mode_can_finish_before_timeout():
    result = command("printf done", timeout_seconds=2, timeout_action="detach")
    assert result.status == "exited"
    assert result.stdout == "done"
    assert result.exit_code == 0


def test_pipe_closure_can_break_detached_program(tmp_path):
    marker = tmp_path / "broken-pipe"
    source = f"""import os, time
from pathlib import Path
time.sleep(0.2)
try:
    os.write(1, b'late output')
except BrokenPipeError:
    Path({str(marker)!r}).write_text('broken')
"""
    result = command(python_command(source), timeout_seconds=0, timeout_action="detach")
    try:
        deadline = time.monotonic() + 5
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        assert marker.read_text() == "broken"
    finally:
        reap_test_child(result.pid)


def test_redirected_detached_command_survives_cli_exit(tmp_path):
    marker = tmp_path / "completed"
    log = tmp_path / "output.log"
    source = f"import time; from pathlib import Path; time.sleep(0.4); Path({str(marker)!r}).write_text('done'); print('output')"
    shell = f"{python_command(source)} </dev/null >{shlex.quote(str(log))} 2>&1"
    result = subprocess.run(
        [
            sys.executable,
            "-m",
            "simplex_ptc",
            "--module",
            "simplex_ptc.tools.process",
            "execute",
            "--state-directory",
            str(tmp_path / "state"),
        ],
        input=json.dumps(
            {
                "execution_id": "one",
                "source": f'run_command({shell!r}, timeout_seconds=0, timeout_action="detach")',
            }
        ),
        text=True,
        capture_output=True,
        timeout=10,
    )
    assert result.returncode == 0, result.stderr
    response = json.loads(result.stdout)
    assert response["result"]["status"] == "detached", response
    deadline = time.monotonic() + 5
    while not marker.exists() and time.monotonic() < deadline:
        time.sleep(0.02)
    assert marker.read_text() == "done"
    # No interpreter-created task files: only caller-selected log and marker.
    assert set(p.name for p in tmp_path.iterdir()) == {"state", "completed", "output.log"}


def test_exited_shell_with_descendant_pipe_has_bounded_drain():
    result = command("sleep 1 & printf early", timeout_seconds=3)
    assert result.status == "exited"
    assert result.stdout == "early"
    assert not result.output_complete
    assert any("before EOF" in hint for hint in result.hints)
    # This deliberately short descendant exits without a long-lived fixture.


@pytest.mark.parametrize(
    "kwargs",
    [
        {"command": ""},
        {"command": "a\0b"},
        {"command": 1},
        {"command": "true", "working_directory": ""},
        {"command": "true", "timeout_action": "invalid"},
        {"command": "true", "timeout_seconds": 0},
        {"command": "true", "timeout_seconds": -1},
        {"command": "true", "timeout_seconds": float("inf")},
        {"command": "true", "timeout_seconds": float("nan")},
        {"command": "true", "timeout_seconds": True},
        {"command": "true", "timeout_seconds": 301},
    ],
)
def test_invalid_arguments_fail_before_launch(kwargs, monkeypatch):
    monkeypatch.setattr(
        impl.subprocess, "Popen", lambda *a, **k: pytest.fail("launched invalid command")
    )
    with pytest.raises(ValueError):
        impl.run_command(**kwargs)


def test_monty_members_manual_persistence_and_no_duplicate_effect(tmp_path):
    registry = ToolRegistry.load(["simplex_ptc.tools.process"])
    session = Session(tmp_path / "state", registry)
    target = tmp_path / "counter"
    request = Request(
        "one",
        f"r = run_command({('printf x >> ' + shlex.quote(str(target)))!r})\nsaved = r.to_dict()\n[r.exit_code, r.render(), saved]",
    )
    first = session.execute(request)
    assert first["status"] == "completed", first
    assert first["result"][0] == 0
    assert session.execute(request) == first
    assert target.read_text() == "x"
    second = session.execute(
        Request("two", '[saved, manual("run_command").to_dict()]', first["epoch"])
    )
    assert second["result"][0] == first["result"][2]
    assert "SIGPIPE" in "\n".join(second["result"][1]["notes"])


def test_kill_cleanup_on_interruption(monkeypatch):
    seen = []
    original = impl.subprocess.Popen

    def launch(*args, **kwargs):
        process = original(*args, **kwargs)
        seen.append(process)
        return process

    monkeypatch.setattr(impl.subprocess, "Popen", launch)
    monkeypatch.setattr(
        impl._Capture, "read_ready", lambda *args: (_ for _ in ()).throw(KeyboardInterrupt())
    )
    with pytest.raises(KeyboardInterrupt):
        command("exec sleep 10")
    assert seen[0].returncode == -signal.SIGKILL
    assert seen[0].stdout.closed and seen[0].stderr.closed


def test_detach_cleanup_on_interruption_does_not_kill(monkeypatch):
    seen = []
    original = impl.subprocess.Popen

    def launch(*args, **kwargs):
        process = original(*args, **kwargs)
        seen.append(process)
        return process

    monkeypatch.setattr(impl.subprocess, "Popen", launch)
    monkeypatch.setattr(
        impl._Capture, "read_ready", lambda *args: (_ for _ in ()).throw(KeyboardInterrupt())
    )
    try:
        with pytest.raises(KeyboardInterrupt):
            command("exec sleep 10", timeout_action="detach")
        assert seen[0].poll() is None
        assert seen[0].stdout.closed and seen[0].stderr.closed
    finally:
        if seen:
            reap_test_child(seen[0].pid)


def test_missing_working_directory_is_a_launch_error(tmp_path):
    with pytest.raises(FileNotFoundError):
        command("printf never", working_directory=str(tmp_path / "missing"))


def test_shell_fallback_and_missing_shell(monkeypatch):
    monkeypatch.setattr(impl.os, "access", lambda path, mode: path == "/bin/sh")
    assert impl._shell() == "/bin/sh"
    monkeypatch.setattr(impl.os, "access", lambda path, mode: False)
    with pytest.raises(FileNotFoundError):
        impl._shell()


def test_failed_python_does_not_undo_detached_work(tmp_path):
    marker = tmp_path / "side-effect"
    shell = f"sleep 0.1; printf done >{shlex.quote(str(marker))}"
    registry = ToolRegistry.load(["simplex_ptc.tools.process"])
    session = Session(tmp_path / "state", registry)
    result = session.execute(
        Request(
            "one",
            f'run_command({shell!r}, timeout_seconds=0, timeout_action="detach")\nraise ValueError("later failure")',
        )
    )
    assert result["status"] == "failed"
    assert not result["committed"]
    deadline = time.monotonic() + 5
    while not marker.exists() and time.monotonic() < deadline:
        time.sleep(0.02)
    assert marker.read_text() == "done"


def test_capture_setup_failure_closes_pipes_and_kills_owned_child(monkeypatch):
    seen = []
    original = impl.subprocess.Popen

    def launch(*args, **kwargs):
        process = original(*args, **kwargs)
        seen.append(process)
        return process

    monkeypatch.setattr(impl.subprocess, "Popen", launch)
    monkeypatch.setattr(
        impl, "_Capture", lambda *args: (_ for _ in ()).throw(OSError("setup failure"))
    )
    with pytest.raises(OSError, match="setup failure"):
        command("exec sleep 10")
    assert seen[0].returncode == -signal.SIGKILL
    assert seen[0].stdout.closed and seen[0].stderr.closed


@pytest.mark.parametrize("through_session", [False, True])
@pytest.mark.parametrize("signum", [signal.SIGINT, signal.SIGTERM])
def test_interrupted_constructor_owns_and_kills_child(
    tmp_path, monkeypatch, through_session, signum
):
    """Deliver a real signal after exec succeeds but before Popen returns."""
    marker = tmp_path / "effect-after-failure"
    source = f"sleep 0.3; printf leaked >{shlex.quote(str(marker))}"
    children = []
    original = subprocess.Popen._execute_child

    def interrupted_launch(self, *args, **kwargs):
        original(self, *args, **kwargs)
        children.append(self)
        signal.raise_signal(signum)

    def interrupt(_signum, _frame):
        raise KeyboardInterrupt("test launch interruption")

    old_handler = signal.signal(signum, interrupt)
    monkeypatch.setattr(subprocess.Popen, "_execute_child", interrupted_launch)
    try:
        if through_session:
            session = Session(tmp_path / "state", ToolRegistry.load(["simplex_ptc.tools.process"]))
            response = session.execute(
                Request("one", f"run_command({source!r}, timeout_seconds=10)")
            )
            assert response["status"] == "failed", response
            assert not response["committed"]
            assert not (tmp_path / "state" / "execution.pending").exists()
            assert session.storage.read("checkpoint")["receipt"]["response"] == response
        else:
            with pytest.raises(KeyboardInterrupt, match="test launch interruption"):
                command(source, timeout_seconds=10)
        assert signal.getsignal(signum) is interrupt
        assert len(children) == 1
        child = children[0]
        assert child.returncode == -signal.SIGKILL
        assert child.stdout.closed and child.stderr.closed
        with pytest.raises(ChildProcessError):
            os.waitpid(child.pid, os.WNOHANG)
        time.sleep(0.4)
        assert not marker.exists()
    finally:
        signal.signal(signum, old_handler)
        for child in children:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait(timeout=3)


def test_constructor_signal_preserves_detach_policy(monkeypatch):
    children = []
    original = subprocess.Popen._execute_child

    def interrupted_launch(self, *args, **kwargs):
        original(self, *args, **kwargs)
        children.append(self)
        signal.raise_signal(signal.SIGINT)

    old_handler = signal.signal(signal.SIGINT, signal.default_int_handler)
    monkeypatch.setattr(subprocess.Popen, "_execute_child", interrupted_launch)
    try:
        with pytest.raises(KeyboardInterrupt):
            command("exec sleep 10", timeout_action="detach")
        assert children[0].poll() is None
        assert children[0].stdout.closed and children[0].stderr.closed
        assert signal.getsignal(signal.SIGINT) is signal.default_int_handler
    finally:
        signal.signal(signal.SIGINT, old_handler)
        for child in children:
            reap_test_child(child.pid)


def test_launch_keeps_child_signal_mask_and_ignore_disposition():
    old_handler = signal.signal(signal.SIGINT, signal.SIG_IGN)
    try:
        result = command(
            python_command(
                "import signal; "
                "print(signal.getsignal(signal.SIGINT) == signal.SIG_IGN); "
                "blocked = signal.pthread_sigmask(signal.SIG_BLOCK, []); "
                "print(signal.SIGINT in blocked, signal.SIGTERM in blocked)"
            )
        )
        assert result.stdout == "True\nFalse False\n"
        assert signal.getsignal(signal.SIGINT) == signal.SIG_IGN
        # The default termination disposition remains functional in the child.
        assert command("kill -TERM $$").termination_signal == signal.SIGTERM
    finally:
        signal.signal(signal.SIGINT, old_handler)


def test_launch_restores_handlers_when_popen_fails(tmp_path):
    original = {signum: signal.getsignal(signum) for signum in (signal.SIGINT, signal.SIGTERM)}
    with pytest.raises(FileNotFoundError):
        command("true", working_directory=str(tmp_path / "missing"))
    assert {signum: signal.getsignal(signum) for signum in original} == original


def test_command_from_non_main_thread_does_not_change_signal_handlers():
    from concurrent.futures import ThreadPoolExecutor

    handlers = {sig: signal.getsignal(sig) for sig in (signal.SIGINT, signal.SIGTERM)}
    with ThreadPoolExecutor(max_workers=1) as executor:
        result = executor.submit(command, "printf thread").result(timeout=5)
    assert result.stdout == "thread"
    assert {sig: signal.getsignal(sig) for sig in handlers} == handlers


def test_nonraising_launch_handler_runs_after_acquisition(monkeypatch):
    children = []
    observed = []
    original = subprocess.Popen._execute_child

    def interrupted_launch(self, *args, **kwargs):
        original(self, *args, **kwargs)
        children.append(self)
        signal.raise_signal(signal.SIGTERM)
        assert not observed

    def handler(signum, _frame):
        observed.append(signum)

    old_handler = signal.signal(signal.SIGTERM, handler)
    monkeypatch.setattr(subprocess.Popen, "_execute_child", interrupted_launch)
    try:
        result = command("printf completed")
        assert result.stdout == "completed"
        assert observed == [signal.SIGTERM]
        assert signal.getsignal(signal.SIGTERM) is handler
    finally:
        signal.signal(signal.SIGTERM, old_handler)
        for child in children:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait(timeout=3)
