"""The only module coupled to the pinned Monty API.

Each call owns exactly one subprocess and one checkout. No filesystem mounts,
OS handler, package installer, or live host objects are exposed to sandbox code.
"""

import sysconfig
from pathlib import Path

import pydantic_monty as monty

from .extensions import ToolRegistry
from .protocol import Output, PTCError
from .resources import execution_resources

# Dumps restore their limits instead of accepting new checkout limits. Include
# this policy in checkpoint compatibility so an upgrade cannot silently weaken it.
LIMITS = {
    "max_feed_duration_secs": 5.0,
    "max_turn_duration_secs": 5.0,
    "max_memory": 64 * 1024 * 1024,
    "max_recursion_depth": 200,
    "max_suspensions": 1000,
    "max_total_sleep_secs": 1.0,
}


def compatibility(registry: ToolRegistry) -> dict:
    """Require exact runtime and policy compatibility before loading opaque bytes."""
    return {"monty": monty.__version__, "tools": registry.fingerprint(), "limits": LIMITS}


def run(
    source: str, state: bytes | None, registry: ToolRegistry, output: Output
) -> tuple[object, bytes]:
    """Execute once and dump idle state; any exception discards this checkout.

    The caller publishes the returned dump only after result conversion succeeds.
    Pool shutdown joins the owned runtime even when load/feed/dump raises.
    """
    binary = Path(sysconfig.get_path("scripts")) / "monty"
    with (
        execution_resources(),
        monty.Monty(binary_path=binary, max_processes=1, request_timeout=10.0) as pool,
    ):
        with pool.checkout(limits=LIMITS) as session:
            if state is not None:
                try:
                    session.load_session(state)
                except Exception as exc:
                    raise PTCError(
                        "incompatible_state", "Monty could not restore the checkpoint"
                    ) from exc
            result = session.feed_run(
                source,
                external_lookup=registry.bindings(),
                print_callback=lambda _stream, text: output.write(text),
            )
            return result, session.dump()
