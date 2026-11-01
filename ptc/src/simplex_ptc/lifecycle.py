"""Daemon-owned resource lifecycle for managed commands and future host adapters."""

import asyncio
import math
import time
import uuid
from dataclasses import asdict, dataclass
from typing import Protocol

from .protocol import PTCError


@dataclass(frozen=True)
class Policy:
    """Operator deadlines, never parameters controlled by submitted Python source."""

    idle_seconds: float = 900
    resource_seconds: float = 3600
    cleanup_seconds: float = 10

    def __post_init__(self):
        for value in asdict(self).values():
            if (
                isinstance(value, bool)
                or not isinstance(value, (int, float))
                or not math.isfinite(value)
                or value <= 0
            ):
                raise ValueError("lifecycle deadlines must be positive finite numbers")


class Resource(Protocol):
    """Future adapters implement nonblocking liveness and cooperative async cleanup.

    close() must tolerate cancellation at the deadline and settle its own tasks.
    A blocking sync cleanup is not a valid implementation. IDs never represent PIDs.
    """

    def alive(self) -> bool:
        """Return whether this resource still requires explicit lifetime ownership."""
        ...

    async def close(self) -> None:
        """Stop and join the owned resource without publishing persistent state."""
        ...


class Resources:
    """Event-loop-affine registry and activity clock, with generation-scoped IDs."""

    def __init__(self, generation: str, policy: Policy, clock=time.monotonic):
        self.generation = generation
        self.policy = policy
        self.clock = clock
        self.last_use = clock()
        self.items: dict[str, Resource] = {}
        self.closing = False

    def touch(self) -> None:
        """Only admitted execution/explicit resource use renews unattended retention."""
        self.last_use = self.clock()

    def add(self, resource: Resource) -> str:
        """Transfer ownership into this daemon; no checkpoint serialization is implied."""
        if self.closing:
            raise PTCError("stopping", "resource registration is closed")
        key = f"{self.generation}:{uuid.uuid4()}"
        self.items[key] = resource
        self.touch()
        return key

    def get(self, key: str) -> Resource:
        """Resolve and renew an explicit use; stale IDs cannot revive old resources."""
        if self.closing or key not in self.items or not self.items[key].alive():
            raise PTCError("resource_lost", "resource is unavailable in this daemon generation")
        self.touch()
        return self.items[key]

    def remove(self, key: str) -> None:
        """Unregister only after the adapter has settled its resource."""
        self.items.pop(key, None)

    def expired(self, running: bool) -> bool:
        """Passive polling/output never renews retention; active executions are exempt."""
        live = any(item.alive() for item in self.items.values())
        timeout = self.policy.resource_seconds if live else self.policy.idle_seconds
        return not running and self.clock() - self.last_use >= timeout

    async def close(self) -> None:
        """Attempt every cleanup concurrently within one deadline, preserving first failure."""
        self.closing = True
        items, self.items = list(self.items.values()), {}

        async def stop(item):
            async with asyncio.timeout(self.policy.cleanup_seconds):
                await item.close()

        results = await asyncio.gather(*(stop(item) for item in items), return_exceptions=True)
        for result in results:
            if isinstance(result, BaseException):
                raise result
