#!/usr/bin/env bash
# Provision source inputs, not compiled binaries. Shared by dependency builds.
# Version and checksum are recorded in third_party/versions/sqlite.md.
set -euo pipefail

destination=${1:-/opt/simplex-thirdparty/include/sqlite}
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT

wget --quiet --timeout=30 --tries=3 \
    -O "$temporary/sqlite.zip" \
    https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip

python3 - "$temporary/sqlite.zip" "$destination" <<'PY'
import hashlib
from pathlib import Path
import sys
import zipfile

archive = Path(sys.argv[1])
destination = Path(sys.argv[2])
expected = "7b670a62fdfbd672b75fef004cb703c8a3e87d3a5cc7d675b4a08337004a2d93"
if hashlib.sha3_256(archive.read_bytes()).hexdigest() != expected:
    raise SystemExit("SQLite 3.54.0 archive SHA3-256 mismatch")

# Read only the four known members, rather than extracting archive-controlled
# paths. Reading all members before writing also verifies their CRCs first.
with zipfile.ZipFile(archive) as source:
    files = {
        name: source.read("sqlite-amalgamation-3540000/" + name)
        for name in ("sqlite3.c", "sqlite3.h", "sqlite3ext.h", "shell.c")
    }
destination.mkdir(parents=True, exist_ok=True)
for name, contents in files.items():
    (destination / name).write_bytes(contents)
print("Verified and installed SQLite 3.54.0 amalgamation in", destination)
PY
