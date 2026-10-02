/**
 * @file shared preconditions for the end-to-end suite.
 *
 * The ordinary real-worker suite needs an executable worker and its prompt.
 * Local runs may skip when they are unavailable. SIMPLEX_E2E_REQUIRED=1 makes
 * missing runtime prerequisites fatal in selected CI runs. The separate Docker
 * lifetime suite remains opt-in through SIMPLEX_DOCKER_WORKER_TEST=1.
 *
 * `SIMPLEX_WORKER_BIN` overrides the in-tree default, which is how CI points
 * the suite at the *staged release* rather than at the build tree. The prompt
 * file is not configurable separately: the worker reads it from its own
 * installation directory.
 */
import { dirname, join } from 'node:path';
import { hubRoot } from '../../src/config.ts';
import { startTestHub } from './hub.js';
import { workerUnavailableReason } from './worker-preconditions.js';

const repoRoot = join(hubRoot, '..');

/** Worker executable under test. */
export const WORKER_BIN = process.env.SIMPLEX_WORKER_BIN
    ?? join(repoRoot, 'build', 'bin', 'simplex_worker');

/** The default role prompt, which the worker reads beside its own executable. */
export const PROMPT_FILE = join(dirname(WORKER_BIN), 'prompts', 'coding_agent.yaml');

const required = process.env.SIMPLEX_E2E_REQUIRED === '1';
const unavailable = workerUnavailableReason(WORKER_BIN, { required });
if (required && unavailable) {
    throw new Error(`Required real-worker E2E unavailable: ${unavailable}`);
}

/** True when a real worker can be started. */
export const e2eAvailable = unavailable === null;

/** `false`, or the reason node:test should skip with. */
export const e2eSkip = e2eAvailable
    ? false
    : `real worker unavailable: ${unavailable}`;

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
