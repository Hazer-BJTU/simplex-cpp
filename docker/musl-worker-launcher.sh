#!/usr/bin/env bash
# Installed as bin/simplex_worker ONLY in a prepared musl package. Keep the
# public simplex command router and the worker's argument parser unchanged.
set -euo pipefail
launcher=${BASH_SOURCE[0]}
links=0
while [[ -L "$launcher" ]]; do
    if (( links >= 40 )); then
        echo 'simplex_worker: too many launcher symbolic links' >&2
        exit 126
    fi
    links=$((links + 1))
    directory=$(cd -P -- "$(dirname -- "$launcher")" && pwd)
    launcher=$(readlink -- "$launcher")
    if [[ "$launcher" != /* ]]; then
        launcher="$directory/$launcher"
    fi
done
directory=$(cd -P -- "$(dirname -- "$launcher")" && pwd)
root=$(cd -- "$directory/.." && pwd)
loader="$root/lib/ld-musl-x86_64.so.1"
worker="$directory/simplex_worker.bin"
if [[ ! -x "$loader" || ! -x "$worker" ]]; then
    echo 'simplex_worker: bundled loader or worker executable is missing or not executable' >&2
    exit 127
fi

# Respect explicit trust configuration, including SSL_CERT_DIR-only setups.
# Prefer host trust (including locally installed corporate roots), with a
# packaged bundle for minimal systems. Never disable peer verification.
if [[ ! ${SSL_CERT_FILE+x} && ! ${SSL_CERT_DIR+x} ]]; then
    for candidate in /etc/ssl/certs/ca-certificates.crt \
                     /etc/pki/tls/certs/ca-bundle.crt /etc/ssl/cert.pem \
                     "$root/share/certs/ca-certificates.crt"; do
        if [[ -r "$candidate" ]]; then
            export SSL_CERT_FILE="$candidate"
            break
        fi
    done
fi
# Do not accidentally load a host-specific FIPS/legacy provider configuration
# into the bundled OpenSSL. Explicit user configuration remains authoritative.
if [[ ! ${OPENSSL_CONF+x} ]]; then
    export OPENSSL_CONF="$root/share/openssl/openssl.cnf"
fi

# This search path belongs to this invocation, not to the environment inherited
# by host commands. exec preserves PID, exit status and signal delivery, and
# the absolute ELF path lets executable::location locate the application.
exec "$loader" --library-path "$root/lib" "$worker" "$@"
