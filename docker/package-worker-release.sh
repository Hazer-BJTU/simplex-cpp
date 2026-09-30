#!/usr/bin/env bash
# Package an already installed worker tree. No build-tree paths enter the asset.
set -euo pipefail

if (( $# != 2 )); then
    echo "usage: $0 STAGING_DIR OUTPUT_DIR" >&2
    exit 2
fi

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
stage=$(cd -- "$1" && pwd)
mkdir -p -- "$2"
output=$(cd -- "$2" && pwd)
version=$(<"$repo/VERSION")
component='(0|[1-9][0-9]*)'
if [[ ! "$version" =~ ^${component}\.${component}\.${component}$ ]]; then
    echo "VERSION must contain three numeric components without leading zeros" >&2
    exit 1
fi
if [[ "$(uname -m)" != x86_64 ]]; then
    echo "this release package targets x86_64 only" >&2
    exit 1
fi
if [[ ! -x "$stage/bin/simplex_worker" || ! -x "$stage/bin/simplex" || ! -d "$stage/lib" ]]; then
    echo "staging tree is missing required worker files" >&2
    exit 1
fi

name="simplex-worker-v${version}-linux-x86_64-glibc2.34"
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
mkdir -- "$tmp/$name"
cp -a -- "$stage/." "$tmp/$name/"
cp -- "$repo/LICENSE" "$tmp/$name/LICENSE"
cp -a -- "$repo/third_party/license" "$tmp/$name/third_party_licenses"
cp -- "$repo/docker/WORKER_RELEASE.md" "$tmp/$name/README.md"

# Stable metadata makes a rebuilt archive comparable with one uploaded by a
# previous workflow attempt for the same tag.
tar -C "$tmp" --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
    --pax-option=delete=atime,delete=ctime -cf - "$name" |
    gzip -n > "$output/$name.tar.gz"
(
    cd -- "$output"
    sha256sum -- "$name.tar.gz" > SHA256SUMS
)
echo "$output/$name.tar.gz"
