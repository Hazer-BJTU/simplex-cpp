/**
 * @file shared preconditions for the end-to-end suite.
 *
 * Both end-to-end files need the same thing: a real `simplex_worker` and the
 * prompt file it loads at startup. Keeping the resolution in one place means
 * the two suites cannot disagree about what "the worker is available" means —
 * and both skip with the same explanation when it is not.
 *
 * `SIMPLEX_WORKER_BIN` and `SIMPLEX_PROMPTS_DIR` override the in-tree defaults,
 * which is how CI points the suite at the *staged release* rather than at the
 * build tree.
 */
import { existsSync } from 'node:fs';
import { join } from 'node:path';
import { hubRoot } from '../../src/config.js';
import { startTestHub } from './hub.js';

const repoRoot = join(hubRoot, '..');

/** Worker executable under test. */
export const WORKER_BIN = process.env.SIMPLEX_WORKER_BIN
    ?? join(repoRoot, 'build', 'bin', 'simplex_worker');

/** Directory holding the worker's prompt files. */
export const PROMPTS_DIR = process.env.SIMPLEX_PROMPTS_DIR
    ?? join(repoRoot, 'build', 'bin', 'prompts');

/** The default role prompt the worker validates at startup. */
export const PROMPT_FILE = join(PROMPTS_DIR, 'coding_agent.yaml');

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
            promptsDir: PROMPTS_DIR,
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
