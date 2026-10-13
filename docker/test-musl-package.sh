#!/usr/bin/env bash
# Run inside the musl build image. Exercise the distributed archive, not just
# a staging tree, and reject an otherwise working package with missing notices.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
stage=${1:-/opt/simplex}
probe=${2:-$repo/docker/musl-launch-probe}
temporary=$(mktemp -d)
trap 'rm -r -- "$temporary"' EXIT
bash "$repo/docker/package-musl-worker.sh" "$stage" "$temporary/output"
(cd "$temporary/output" && sha256sum -c *.sha256)
mkdir "$temporary/extracted"
tar -xzf "$temporary/output/"*.tar.gz -C "$temporary/extracted"
python3 "$repo/docker/test-musl-runtime.py" \
    "$temporary/extracted/simplex-worker-v$(cat "$repo/VERSION")-linux-x86_64-musl" \
    "$repo" "$probe"
cp -R -P --preserve=mode,timestamps "$stage" "$temporary/incomplete"
rm "$temporary/incomplete/share/licenses/GCC-COPYING.RUNTIME"
if bash "$repo/docker/package-musl-worker.sh" "$temporary/incomplete" "$temporary/rejected"; then
    echo 'packager accepted a missing runtime-library license exception' >&2
    exit 1
fi
if find "$temporary/rejected" -type f | grep -q .; then
    echo 'failed packaging left an output artifact' >&2
    exit 1
fi
printf 'musl archive, relocation and missing-license rejection passed\n'
