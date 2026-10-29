/** Exercise the installed-YAML override block without rebuilding a toolchain. */
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { spawnSync } from 'node:child_process';
import { test } from 'node:test';

const script = readFileSync(new URL('../../docker/check-musl-portable.sh', import.meta.url), 'utf8');
const start = script.indexOf('declaration="$stage/bin/schemas/reading/read_text.yaml"');
const end = script.indexOf('run_test test_reading_tools --run_test=selected_yaml_configuration_controls_read_limits', start);
assert.ok(start >= 0 && end > start, 'installed configuration test block must exist');
const block = script.slice(start, end);
const current = readFileSync(new URL('../../tools/intrinsic/toolsets/reading/schemas/read_text.yaml', import.meta.url), 'utf8');

for (const [name, yaml] of [
    ['current shipped defaults', current],
    ['changed defaults', current.replace(/max_file_bytes: \d+/, 'max_file_bytes: 33554432')
        .replace(/max_output_bytes: \d+/, 'max_output_bytes: 8192')],
    ['missing setting', current.replace(/^  max_file_bytes: .*\n/m, '')],
]) {
    test(`musl installed override: ${name}`, t => {
        const stage = mkdtempSync(join(tmpdir(), 'musl config '));
        t.after(() => rmSync(stage, { recursive: true, force: true }));
        mkdirSync(join(stage, 'bin/schemas/reading'), { recursive: true });
        const file = join(stage, 'bin/schemas/reading/read_text.yaml');
        writeFileSync(file, yaml);
        const result = spawnSync('bash', ['-euo', 'pipefail', '-c', block], {
            env: { ...process.env, stage }, encoding: 'utf8',
        });
        if (name === 'missing setting') {
            assert.notEqual(result.status, 0, 'a missing setting must not silently skip override validation');
        } else {
            assert.equal(result.status, 0, result.stderr);
            assert.equal(readFileSync(file, 'utf8'), yaml
                .replace(/^  max_file_bytes: \d+$/m, '  max_file_bytes: 64')
                .replace(/^  max_output_bytes: \d+$/m, '  max_output_bytes: 7'));
        }
    });
}
