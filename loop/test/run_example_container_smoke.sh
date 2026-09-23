#!/bin/sh

# The configured CTest tree travels between release and target containers.
# Its Python interpreter path cannot travel with it. Check at test time, and
# keep the actual process experiment confined to a disposable container.
if [ ! -f /.dockerenv ]; then
    echo 'SKIP: process example runs only inside Docker'
    exit 77
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo 'SKIP: Python 3 is unavailable in this runtime image'
    exit 77
fi

exec python3 "$@"
