"""Single-request CLI. Machine commands emit JSON; repl provides a separate human console."""

import argparse
import contextlib
import json
import signal
import sys
from pathlib import Path

from .client import Client
from .extensions import ToolRegistry
from .lifecycle import Policy
from .protocol import (
    MAX_REQUEST_BYTES,
    MAX_RESPONSE_BYTES,
    ExecutionRejected,
    PTCError,
    Request,
    encode,
    error_response,
)
from .runtime import LIMITS, compatibility


def parser() -> argparse.ArgumentParser:
    """Build the standalone CLI without importing any worker configuration."""
    root = argparse.ArgumentParser(prog="simplex-ptc")
    root.add_argument(
        "--module", action="append", default=[], help="trusted installed extension module"
    )
    root.add_argument(
        "--idle-seconds",
        type=float,
        default=900,
        help="daemon idle deadline without live resources (default: 900)",
    )
    root.add_argument(
        "--resource-seconds",
        type=float,
        default=3600,
        help="unattended live-resource retention (default: 3600)",
    )
    root.add_argument(
        "--cleanup-seconds",
        type=float,
        default=10,
        help="cooperative runner/resource cleanup deadline (default: 10)",
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
    daemon = commands.add_parser("daemon", help="inspect or stop the automatically managed daemon")
    actions = daemon.add_subparsers(dest="operation", required=True)
    for name in ("status", "stop"):
        action = actions.add_parser(name)
        action.add_argument("--state-directory", type=Path, required=True)
    for name in ("query", "cancel"):
        action = operations.add_parser(name)
        action.add_argument("--state-directory", type=Path, required=True)
        action.add_argument("--execution-id", required=True)
        action.add_argument("--epoch")
        if name == "cancel":
            action.add_argument("--generation", required=True)
    return root


def policy_from_args(args: argparse.Namespace) -> Policy:
    """Reject invalid operator deadlines before a daemon or execution is started."""
    try:
        return Policy(args.idle_seconds, args.resource_seconds, args.cleanup_seconds)
    except ValueError as exc:
        raise PTCError("invalid_policy", str(exc)) from exc


def dispatch(args: argparse.Namespace) -> dict:
    """Resolve operator modules before execution; no model-controlled imports."""
    if args.command == "capabilities":
        registry = ToolRegistry.load(args.module)
        return {
            "protocol": 1,
            "runtime": compatibility(registry),
            "tools": registry.manifest(),
            "limits": LIMITS,
            "max_request_bytes": MAX_REQUEST_BYTES,
            "max_response_bytes": MAX_RESPONSE_BYTES,
        }
    session = Client(args.state_directory, args.module, policy_from_args(args))
    if args.command == "daemon":
        return session.stop() if args.operation == "stop" else session.call("status", start=False)
    if args.command == "session" and args.operation in ("query", "cancel"):
        fields = {"execution_id": args.execution_id, "epoch": args.epoch}
        if args.operation == "cancel":
            fields["generation"] = args.generation
        return session.call(args.operation, start=False, **fields)
    if args.command == "execute":
        raw = sys.stdin.buffer.read(MAX_REQUEST_BYTES + 1)
        if len(raw) > MAX_REQUEST_BYTES:
            raise ExecutionRejected("invalid_request", "request exceeds byte limit")
        try:
            value = json.loads(raw)
        except (ValueError, UnicodeError, RecursionError) as exc:
            raise ExecutionRejected(
                "invalid_request", "stdin must contain one JSON request"
            ) from exc
        return session.execute(Request.parse(value))
    if args.operation == "recover":
        session.recover(args.epoch, args.execution_id)
    elif args.operation == "reset":
        session.reset(args.epoch)
    return session.inspect()


def main() -> int:
    """Return 0 for success, 1 for execution failure, 2 for rejection/boundary failure.

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

            try:
                policy = policy_from_args(args)
            except PTCError as exc:
                print(f"[error: {exc.code}] {exc}", file=sys.stderr)
                return 2
            return run_console(args.state_directory, args.module, policy)
        try:
            with contextlib.redirect_stdout(sys.stderr):
                response = dispatch(args)
            code = 1 if response.get("status") == "failed" else 0
        except PTCError as exc:
            response = error_response(exc)
            response.update(getattr(exc, "response", {}))
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
                    "commit_status": "unknown",
                    **{
                        key: response[key]
                        for key in ("execution_id", "epoch", "session_id", "commands_created")
                        if key in response
                    },
                    "error": {"code": "response_limit", "message": "response exceeds budget"},
                }
            )
            code = 2
        sys.stdout.buffer.write(data + b"\n")
        sys.stdout.buffer.flush()
        return code
    finally:
        signal.signal(signal.SIGTERM, old)
