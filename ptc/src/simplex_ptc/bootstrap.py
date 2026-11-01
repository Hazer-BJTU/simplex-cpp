"""Start internal Python modules from a stable cwd with the caller's import paths."""

import json
import os
import sys


def module_command(module: str, *arguments: str) -> list[str]:
    """Anchor relative search entries without changing the inherited environment.

    A daemon or runner starts from '/', but its configured extensions must resolve
    exactly as in the owning Python process. Keep the original environment for
    extension configuration and restore per-execution context separately.
    """
    cwd = os.getcwd()
    paths = [
        entry if os.path.isabs(entry) else os.path.join(cwd, entry)
        for entry in sys.path
        if isinstance(entry, str)
    ]
    return [
        sys.executable,
        "-c",
        "import json, runpy, sys; sys.path[:] = json.loads(sys.argv.pop(1)); "
        "runpy.run_module(sys.argv.pop(1), run_name='__main__', alter_sys=True)",
        json.dumps(paths),
        module,
        *arguments,
    ]
