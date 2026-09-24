#!/bin/sh
# Resolve the interpreter at test time so staged cross-distro CTest works.
if ! command -v python3 >/dev/null 2>&1; then
    echo 'SKIP: Python 3 is unavailable'
    exit 77
fi
exec python3 "$@"
