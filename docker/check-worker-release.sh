#!/usr/bin/env bash
# Check the actual downloadable archive after extraction on a target system.
set -euo pipefail

if (( $# != 1 )); then
    echo "usage: $0 WORKER_ARCHIVE" >&2
    exit 2
fi

archive=$(realpath -- "$1")
name=$(basename -- "$archive" .tar.gz)
[[ "$name" =~ ^simplex-worker-v[0-9]+\.[0-9]+\.[0-9]+-linux-x86_64-glibc2\.34$ ]]
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
tar -tzf "$archive" | while IFS= read -r entry; do
    case "$entry" in
        "$name"|"$name/"|"$name/"*) ;;
        *) echo "unexpected archive member: $entry" >&2; exit 1 ;;
    esac
    if [[ "/$entry/" == *"/../"* || "/$entry/" == *"/./"* ]]; then
        echo "unsafe archive member: $entry" >&2
        exit 1
    fi
done
tar -C "$tmp" -xzf "$archive"
root="$tmp/$name"
test -x "$root/bin/simplex"
test -x "$root/bin/simplex_worker"
test -s "$root/bin/prompts/coding_agent.yaml"
test -s "$root/bin/schemas/process/skill.yaml"
test -s "$root/LICENSE"
test -s "$root/README.md"
test -d "$root/third_party_licenses"
test -d "$root/bin/plugins/llm"
test -s "$root/bin/plugins/llm/libllm_qwen.so"
test -d "$root/lib"

# Run without relying on a development checkout or the build container's
# library search path. This also exercises the installed command router.
env -u LD_LIBRARY_PATH "$root/bin/simplex" run --help >/dev/null
if command -v ldd >/dev/null; then
    env -u LD_LIBRARY_PATH ldd -r "$root/bin/simplex_worker" >"$tmp/ldd.out" 2>&1
    if grep -Eq 'not found|undefined symbol' "$tmp/ldd.out"; then
        cat "$tmp/ldd.out" >&2
        exit 1
    fi
fi
echo "validated $name"
