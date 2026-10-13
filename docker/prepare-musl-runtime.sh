#!/usr/bin/env bash
# Turn a CMake installation into a relocatable musl worker tree. Run inside
# the musl build image; never run this on an existing glibc release.
set -euo pipefail
if (( $# != 1 )); then
    echo "usage: $0 STAGING_DIR" >&2
    exit 2
fi
stage=$(cd -- "$1" && pwd)
scripts=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
worker="$stage/bin/simplex_worker"
if [[ -e "$worker.bin" ]]; then
    echo 'musl runtime is already prepared; use a fresh staging directory' >&2
    exit 1
fi
readelf -l "$worker" | grep -q '/ld-musl-x86_64.so.1'
test -f "$stage/lib/ld-musl-x86_64.so.1"
test -f "$stage/lib/libssl.so.3"
test -f "$stage/lib/libcrypto.so.3"

chmod 0755 "$stage/lib/ld-musl-x86_64.so.1"
mv -- "$worker" "$worker.bin"
install -m 0755 "$scripts/musl-worker-launcher.sh" "$worker"
mkdir -p "$stage/share/certs" "$stage/share/openssl"
cp /etc/ssl/cert.pem "$stage/share/certs/ca-certificates.crt"
cat > "$stage/share/openssl/openssl.cnf" <<'CONFIG'
# The default provider is built into libcrypto. No external modules are needed
# for this worker's ordinary TLS use. FIPS and legacy providers are not bundled.
openssl_conf = simplex_init
[simplex_init]
providers = simplex_providers
[simplex_providers]
default = simplex_default
[simplex_default]
activate = 1
CONFIG
cp /opt/simplex-runtime-packages.txt "$stage/runtime-packages.txt"

cp -R -P --preserve=mode,timestamps /opt/simplex-runtime-licenses "$stage/share/licenses"
# Both unpacked installations and container images retain notices, not only
# the final tarball. The caller must provide the repository beside this script.
cp "$scripts/../LICENSE" "$stage/LICENSE"
cp -R -P --preserve=mode,timestamps "$scripts/../third_party/license" "$stage/third_party_licenses"
