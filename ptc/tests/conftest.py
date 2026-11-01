"""Keep standalone CLI tests from leaving automatically started daemons behind."""

import shutil
import tempfile
from pathlib import Path

import pytest

from simplex_ptc.client import Client
from simplex_ptc.protocol import PTCError


@pytest.fixture(autouse=True)
def private_runtime(tmp_path, monkeypatch):
    """Use a short private endpoint root and stop legacy CLI fixtures on teardown."""
    runtime = Path(tempfile.mkdtemp(prefix="ptc-test-"))
    monkeypatch.setenv("XDG_RUNTIME_DIR", str(runtime))
    yield runtime
    for lock in tmp_path.rglob("daemon.lock"):
        for modules in ([], ["simplex_ptc.tools.process"], ["simplex_ptc.tools.files"]):
            client = Client(lock.parent, modules)
            if not client.endpoint.socket.exists():
                break
            try:
                client.stop()
                break
            except PTCError as exc:
                if exc.code != "daemon_incompatible":
                    raise
        else:
            pytest.fail(f"test left an incompatible daemon at {lock.parent}")
    assert not list(runtime.rglob("*.sock")), "test left a live daemon endpoint"
    shutil.rmtree(runtime)
