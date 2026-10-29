/** Compare the freshly built/staged worker, never a downloaded release. */
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { it } from 'node:test';
import { WORKER_BIN, e2eSkip } from '../helpers/e2e.js';
import { manifest } from '../helpers/worker-contract.js';

it('built worker exports the same contract used by Node adapters', { skip: e2eSkip }, () => {
    const options = { encoding: 'utf8', cwd: tmpdir(), timeout: 15000, maxBuffer: 4 * 1024 * 1024 };
    assert.deepEqual(JSON.parse(execFileSync(WORKER_BIN, ['--protocol'], options)), manifest);
    assert.equal(execFileSync(WORKER_BIN, ['--protocol-hash'], options), manifest.fingerprint + '\n');
});
