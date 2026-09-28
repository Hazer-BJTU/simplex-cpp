/**
 * @file shared preconditions for the end-to-end suite.
 *
 * Both end-to-end files need the same thing: a real `simplex_worker` and the
 * prompt file it loads at startup. Keeping the resolution in one place means
 * the two suites cannot disagree about what "the worker is available" means —
 * and both skip with the same explanation when it is not.
 *
 * `SIMPLEX_WORKER_BIN` overrides the in-tree default, which is how CI points
 * the suite at the *staged release* rather than at the build tree. The prompt
 * file is not configurable separately: the worker reads it from its own
 * installation directory.
 */
import { existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { hubRoot } from '../../src/config.ts';
import { startTestHub } from './hub.js';

const repoRoot = join(hubRoot, '..');

/** Worker executable under test. */
export const WORKER_BIN = process.env.SIMPLEX_WORKER_BIN
    ?? join(repoRoot, 'build', 'bin', 'simplex_worker');

/** The default role prompt, which the worker reads beside its own executable. */
export const PROMPT_FILE = join(dirname(WORKER_BIN), 'prompts', 'coding_agent.yaml');

/** True when a real worker can be started. */
export const e2eAvailable = existsSync(WORKER_BIN) && existsSync(PROMPT_FILE);

/** `false`, or the reason node:test should skip with. */
export const e2eSkip = e2eAvailable
    ? false
    : `real worker not built: ${WORKER_BIN} / ${PROMPT_FILE}`;

/**
 * Start a hub that runs the real worker against the offline mock provider.
 *
 * @param {object} [overrides] extra hub configuration.
 */
export async function startE2eHub(overrides = {}) {
    return startTestHub({
        worker: {
            bin: WORKER_BIN,
            threads: 2,
            confirmationTimeoutMs: 60000,
            stopTimeoutMs: 20000,
            persistence: { enabled: true, readable: true },
            ...(overrides.worker ?? {}),
        },
        mock: { enabled: true, ...(overrides.mock ?? {}) },
        ...overrides,
    });
}
