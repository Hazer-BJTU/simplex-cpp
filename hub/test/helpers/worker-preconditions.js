/** Real-worker prerequisites shared by local skipping and strict CI admission. */
import { accessSync, constants, statSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { dirname, join } from 'node:path';

/** Return the unavailable prerequisite, or null. Strict CI also probes loading. */
export function workerUnavailableReason(worker, { required = false } = {}) {
    const bin = dirname(worker);
    const prerequisites = [
        [worker, constants.R_OK | constants.X_OK],
        [join(bin, 'prompts', 'coding_agent.yaml'), constants.R_OK],
    ];
    if (required) {
        // These are actual inputs of the offline mock/compact/process E2E.
        // Do not require optional Docker or browser acceptance prerequisites.
        prerequisites.push(
            ...['compact', 'auto_compact', 'auto_compact_continue'].map(name =>
                [join(bin, 'prompts', 'operations', `${name}.yaml`), constants.R_OK]),
            [join(bin, 'plugins', 'llm', 'libllm_deepseek.so'), constants.R_OK],
            ...['poll_process', 'read_process', 'run_command', 'send_process',
                'spawn_process', 'skill'].map((name) => [
                join(bin, 'schemas', 'process', `${name}.yaml`), constants.R_OK,
            ]),
        );
    }
    for (const [file, mode] of prerequisites) {
        try {
            accessSync(file, mode);
            if (!statSync(file).isFile()) {
                return `not a regular file: ${file}`;
            }
        } catch {
            return `missing or inaccessible prerequisite: ${file}`;
        }
    }
    if (required) {
        // Existence is insufficient: unresolved shared libraries or a damaged
        // executable must fail before node:test can mark the suite skipped.
        const probe = spawnSync(worker, ['--help'], { encoding: 'utf8', timeout: 10000 });
        if (probe.error || probe.status !== 0) {
            return `worker cannot start: ${worker}: ${probe.error?.message
                ?? probe.stderr?.trim() ?? `exit ${probe.status}`}`;
        }
    }
    return null;
}
