"""Single-request CLI. Machine commands emit JSON; repl provides a separate human console."""

import argparse
import contextlib
import json
import signal
import sys
from pathlib import Path

from .extensions import ToolRegistry
from .protocol import MAX_REQUEST_BYTES, MAX_RESPONSE_BYTES, PTCError, Request, encode
from .runtime import LIMITS, compatibility
from .session import Session


def parser() -> argparse.ArgumentParser:
    """Build the standalone CLI without importing any worker configuration."""
    root = argparse.ArgumentParser(prog="simplex-ptc")
    root.add_argument(
        "--module", action="append", default=[], help="trusted installed extension module"
    )
    commands = root.add_subparsers(dest="command", required=True)
    repl = commands.add_parser("repl", help="interactive manual-testing console")
    repl.add_argument(
        "--state-directory", type=Path, help="persist state here; default: temporary session"
    )
    commands.add_parser("capabilities", help="describe the actual runtime and tool registry")
    execute = commands.add_parser("execute", help="execute one JSON request from stdin")
    execute.add_argument("--state-directory", type=Path, required=True)
    session = commands.add_parser("session", help="inspect or explicitly manage idle state")
    operations = session.add_subparsers(dest="operation", required=True)
    for name in ("inspect", "recover", "reset"):
        operation = operations.add_parser(name)
        operation.add_argument("--state-directory", type=Path, required=True)
        if name != "inspect":
            operation.add_argument("--epoch", required=True)
        if name == "recover":
            operation.add_argument("--execution-id", required=True)
    return root


def dispatch(args: argparse.Namespace) -> dict:
    """Resolve operator modules before execution; no model-controlled imports."""
    registry = ToolRegistry.load(args.module)
    if args.command == "capabilities":
        return {
            "protocol": 1,
            "runtime": compatibility(registry),
            "tools": registry.manifest(),
            "limits": LIMITS,
            "max_request_bytes": MAX_REQUEST_BYTES,
            "max_response_bytes": MAX_RESPONSE_BYTES,
        }
    session = Session(args.state_directory, registry)
    if args.command == "execute":
        raw = sys.stdin.buffer.read(MAX_REQUEST_BYTES + 1)
        if len(raw) > MAX_REQUEST_BYTES:
            raise PTCError("invalid_request", "request exceeds byte limit")
        try:
            value = json.loads(raw)
        except (ValueError, UnicodeError, RecursionError) as exc:
            raise PTCError("invalid_request", "stdin must contain one JSON request") from exc
        return session.execute(Request.parse(value))
    if args.operation == "recover":
        session.recover(args.epoch, args.execution_id)
    elif args.operation == "reset":
        session.reset(args.epoch)
    return session.inspect()


def main() -> int:
    """Return 0 for success, 1 for execution failure, 2 for boundary failure.

    SIGTERM uses the same unwinding path as Ctrl-C. SIGKILL cannot be handled;
    an unmatched pending marker then requires explicit recovery. Storage errors
    are conservatively reported with unknown commit status.
    """
    args = parser().parse_args()

    def interrupt(_signal, _frame):
        if args.command == "repl":
            # SIGTERM must exit the console rather than be mistaken for the
            # interactive Ctrl-C action that merely clears input.
            raise SystemExit(128 + _signal)
        raise KeyboardInterrupt("execution interrupted")

    old = signal.signal(signal.SIGTERM, interrupt)
    try:
        if args.command == "repl":
            from .repl import run_console

            return run_console(args.state_directory, args.module)
        try:
            with contextlib.redirect_stdout(sys.stderr):
                response = dispatch(args)
            code = 1 if response.get("status") == "failed" else 0
        except PTCError as exc:
            response = {
                "protocol": 1,
                "status": "failed",
                "error": {"code": exc.code, "message": str(exc)},
            }
            code = 2
        except (OSError, KeyboardInterrupt):
            response = {
                "protocol": 1,
                "status": "failed",
                "commit_status": "unknown",
                "error": {
                    "code": "storage_or_interruption",
                    "message": "inspect session before retrying",
                },
            }
            code = 2
        except Exception as exc:
            print(f"interpreter failure: {type(exc).__name__}", file=sys.stderr)
            response = {
                "protocol": 1,
                "status": "failed",
                "commit_status": "unknown",
                "error": {
                    "code": "interpreter_error",
                    "message": "inspect session before retrying",
                },
            }
            code = 2
        data = encode(response)
        if len(data) > MAX_RESPONSE_BYTES:
            data = encode(
                {
                    "protocol": 1,
                    "status": "failed",
                    "error": {"code": "response_limit", "message": "response exceeds budget"},
                }
            )
            code = 2
        sys.stdout.buffer.write(data + b"\n")
        sys.stdout.buffer.flush()
        return code
    finally:
        signal.signal(signal.SIGTERM, old)
