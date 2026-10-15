#!/usr/bin/env bash
# Shared archive/staging contract, also implemented by the Hub installer:
# at least one libllm_<name>.so, and every matching entry a nonempty file.
# This checks presence, not ABI compatibility or successful plugin loading.
set -euo pipefail
if (( $# != 1 )); then
    echo "usage: $0 WORKER_ROOT" >&2
    exit 2
fi
plugins="$1/bin/plugins/llm"
if [[ ! -d "$plugins" || -L "$plugins" ]]; then
    echo 'worker installation contains no model-provider directory' >&2
    exit 1
fi
shopt -s nullglob
count=0
for provider in "$plugins"/libllm_*.so; do
    [[ "${provider##*/}" != libllm_.so ]] || continue
    if [[ ! -f "$provider" || ! -s "$provider" ]]; then
        printf 'invalid model-provider plugin: %s\n' "$provider" >&2
        exit 1
    fi
    count=$((count + 1))
done
if (( count == 0 )); then
    echo 'worker installation contains no model-provider plugin' >&2
    exit 1
fi
