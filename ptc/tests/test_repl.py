"""Console tests use the real package, no worker/Hub integration."""

import subprocess
import sys

from simplex_ptc import Session
from simplex_ptc.repl import interact, show


def console(source, directory=None):
    args = [sys.executable, "-m", "simplex_ptc", "repl"]
    if directory is not None:
        args.extend(["--state-directory", str(directory)])
    return subprocess.run(args, input=source, text=True, capture_output=True, timeout=15)


def test_temporary_console_manual_multiline_and_failure():
    result = console("manual()\n:paste\nx = 40\nx + 2\n:run\n1 / 0\nx\n:quit\n")
    assert result.returncode == 0, result.stderr
    assert "Temporary test session" in result.stdout
    assert '"name": "manual"' in result.stdout
    assert "[result]\n42" in result.stdout
    assert "[error: runtime_error]" in result.stdout
    assert "[result]\n40" in result.stdout


def test_persistent_console_restores_between_processes(tmp_path):
    assert console("x = 7\n:quit\n", tmp_path).returncode == 0
    result = console("x\n:state\n:quit\n", tmp_path)
    assert result.returncode == 0
    assert "[result]\n7" in result.stdout
    assert '"revision": 2' in result.stdout


def test_abort_and_eof_do_not_execute_buffer(tmp_path):
    result = console(":paste\nx = 9\n:abort\n:paste\nx = 10\n", tmp_path)
    assert result.returncode == 0
    assert not (tmp_path / "checkpoint").exists()


def test_keyboard_interrupt_discards_input(tmp_path, monkeypatch, capsys):
    events = iter([":paste", "x = 9", KeyboardInterrupt(), ":state", ":quit"])

    def read(_prompt):
        value = next(events)
        if isinstance(value, BaseException):
            raise value
        return value

    monkeypatch.setattr("builtins.input", read)
    assert interact(Session(tmp_path)) == 0
    assert "[interrupted" in capsys.readouterr().out
    assert not (tmp_path / "checkpoint").exists()


def test_display_prints_and_truncation(capsys):
    show(
        {
            "output": "hello",
            "output_omitted_bytes": 3,
            "status": "completed",
            "result_truncated": True,
            "result_preview": "preview",
            "revision": 1,
            "committed": True,
        }
    )
    output = capsys.readouterr().out
    assert "[output]\nhello\n" in output
    assert "3 bytes omitted" in output
    assert "[result preview" in output


def test_manual_render_in_console():
    result = console('print(manual("manual").render())\n:quit\n')
    assert result.returncode == 0, result.stderr
    assert "Return methods:" in result.stdout
    assert "[output]" in result.stdout
