#!/usr/bin/env bash
# Package a prepared musl tree separately from the existing glibc release path.
set -euo pipefail
if (( $# != 2 )); then
    echo "usage: $0 PREPARED_STAGING_DIR OUTPUT_DIR" >&2
    exit 2
fi
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
stage=$(cd -- "$1" && pwd)
mkdir -p -- "$2"
output=$(cd -- "$2" && pwd)
version=$(<"$repo/VERSION")
if [[ ! "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
    echo 'invalid repository VERSION' >&2
    exit 1
fi
for executable in bin/simplex bin/simplex_worker bin/simplex_worker.bin lib/ld-musl-x86_64.so.1; do
    test -x "$stage/$executable"
done
# Reject unprepared or foreign installations instead of mislabeling an asset.
cmp "$repo/docker/musl-worker-launcher.sh" "$stage/bin/simplex_worker"
readelf -l "$stage/bin/simplex_worker.bin" | grep -q '/ld-musl-x86_64.so.1'
test "$("$stage/bin/simplex_worker" --version)" = "$version"
# Every ELF dependency must be carried in this package; do not silently use
# libraries installed in the build image. Modules loaded with dlopen count too.
while IFS= read -r -d '' file; do
    if ! readelf -h "$file" >/dev/null 2>&1; then
        continue
    fi
    while IFS= read -r dependency; do
        if [[ ! -f "$stage/lib/$dependency" ]]; then
            printf 'missing bundled dependency %s (required by %s)\n' "$dependency" "$file" >&2
            exit 1
        fi
    done < <(readelf -d "$file" | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
done < <(find "$stage/bin" "$stage/lib" -type f -print0)
for notice in LICENSE third_party_licenses/boost/LICENSE_1_0.txt \
    third_party_licenses/nlohmann_json/LICENSE.MIT third_party_licenses/yaml-cpp/LICENSE \
    share/licenses/musl-COPYRIGHT share/licenses/OpenSSL-LICENSE.txt \
    share/licenses/GCC-COPYING3 share/licenses/GCC-COPYING.RUNTIME \
    share/licenses/Mozilla-MPL-2.0.txt share/licenses/README.md; do
    if [[ ! -s "$stage/$notice" ]]; then
        printf 'missing or empty license notice: %s\n' "$notice" >&2
        exit 1
    fi
done
name="simplex-worker-v${version}-linux-x86_64-musl"
temporary=$(mktemp -d)
trap 'rm -r -- "$temporary"' EXIT
mkdir "$temporary/$name"
cp -R -P --preserve=mode,timestamps "$stage/." "$temporary/$name/"
cp "$repo/docker/MUSL_EXPERIMENT.md" "$temporary/$name/README.md"
tar -C "$temporary" --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
    --pax-option=delete=atime,delete=ctime -cf - "$name" | gzip -n > "$temporary/$name.tar.gz"
mv -- "$temporary/$name.tar.gz" "$output/$name.tar.gz"
(cd "$output" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
printf '%s\n' "$output/$name.tar.gz"
