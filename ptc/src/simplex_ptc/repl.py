"""Small human-facing console over the same persistent execution engine.

No host eval/exec or Python compilation is used. Every submitted snippet follows
normal Monty execution, result conversion and checkpoint rules.
"""

import contextlib
import json
import sys
import tempfile
import uuid
from pathlib import Path

from .extensions import ToolRegistry
from .protocol import PTCError, Request
from .session import Session

HELP = """Enter one line of Python and press Enter to execute.
:paste   collect multiline code; :run submits it, :abort discards it
:state   show session metadata
:help    show this help
:quit    exit (Ctrl-D also exits; unfinished input is discarded)
Ctrl-C clears pending input or interrupts execution. No automatic retry/recovery.
Host result methods do not survive between submissions: use
print(manual("manual").render()) or save manual("manual").to_dict()."""


def show(response: dict) -> None:
    """Separate captured prints, selected results and failure diagnostics."""
    output = response.get("output", "")
    if output:
        print("[output]")
        print(output, end="" if output.endswith("\n") else "\n")
    omitted = response.get("output_omitted_bytes", 0)
    if omitted:
        print(f"[output truncated: {omitted} bytes omitted]")
    if response.get("status") == "failed":
        error = response["error"]
        print(f"[error: {error['code']}] {error['message']}")
    elif response.get("result_truncated"):
        print("[result preview — truncated]")
        print(response["result_preview"])
    elif response.get("result") is not None:
        value = response["result"]
        print("[result]")
        print(value if isinstance(value, str) else json.dumps(value, ensure_ascii=False, indent=2))
    print(
        f"[revision {response['revision']}; {'committed' if response['committed'] else 'not committed'}]"
    )


def interact(session: Session) -> int:
    """Submit unique requests, refreshing epoch without replaying uncertain work."""
    print(HELP)
    lines = None
    while True:
        try:
            line = input("... " if lines is not None else "ptc> ")
            if line == ":quit":
                return 0
            if line == ":abort":
                lines = None
                print("[input discarded]")
                continue
            if lines is not None:
                if line != ":run":
                    lines.append(line)
                    continue
                source = "\n".join(lines)
                lines = None
            elif line == ":paste":
                lines = []
                continue
            elif line == ":help":
                print(HELP)
                continue
            elif line == ":state":
                print(json.dumps(session.inspect(), indent=2))
                continue
            elif line.startswith(":"):
                print("[unknown console command; use :help]")
                continue
            else:
                source = line
            if not source.strip():
                continue
            info = session.inspect()
            request = Request(str(uuid.uuid4()), source, info["epoch"])
            # Trusted extension prints remain diagnostics, not selected results.
            with contextlib.redirect_stdout(sys.stderr):
                response = session.execute(request)
            show(response)
        except EOFError:
            print("\n[exited; unfinished input discarded]")
            return 0
        except KeyboardInterrupt:
            lines = None
            print("\n[interrupted; use :state before continuing if execution was active]")
        except (PTCError, OSError) as exc:
            label = exc.code if isinstance(exc, PTCError) else "storage_error"
            print(f"[error: {label}] {exc}")
            print("[no retry performed; use :state; recover/reset via the session CLI if needed]")


def run_console(directory: Path | None, modules: list[str]) -> int:
    """Use an explicitly persistent directory or a disposable test session."""
    try:
        registry = ToolRegistry.load(modules)
        if directory is not None:
            print(f"Persistent session: {directory.absolute()}")
            return interact(Session(directory, registry))
        with tempfile.TemporaryDirectory(prefix="simplex-ptc-repl-") as temporary:
            print("Temporary test session; state will be removed on exit.")
            return interact(Session(Path(temporary), registry))
    except Exception as exc:
        print(f"[console error: {type(exc).__name__}] {exc}", file=sys.stderr)
        return 2
