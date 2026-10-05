#!/usr/bin/env node
/** Verify the packed artifact rather than trusting the working tree layout. */
import { execFileSync } from 'node:child_process';
import { copyFileSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

if (process.argv.length !== 3) {
    process.stderr.write('Usage: node scripts/check-release-package.mjs PACKAGE.tgz\n');
    process.exit(2);
}

const archive = resolve(process.argv[2]);
const members = execFileSync('tar', ['-tzf', archive], { encoding: 'utf8' })
    .trimEnd().split('\n');
const allowed = [
    'package/LICENSE',
    'package/README.md',
    'package/package.json',
    'package/dist/',
];
for (const member of members) {
    if (member.startsWith('package/dist/docs/') && !member.endsWith('/')
        && !member.endsWith('.md')) {
        throw new Error(`Non-documentation file in packaged docs: ${member}`);
    }
    if (member.endsWith('/panel-redesign-plan.md')) {
        throw new Error('Maintainer design notes must not enter the npm package');
    }
    if (!allowed.some(prefix => member === prefix ||
        (prefix.endsWith('/') && member.startsWith(prefix)))) {
        throw new Error(`Unexpected file in npm package: ${member}`);
    }
}
for (const required of [
    'package/LICENSE',
    'package/dist/docs/core/worker-protocol.md',
    'package/dist/docs/getting-started/installation.md',
    'package/dist/docs/deployment/hub.md',
    'package/dist/docs/plugins/development.md',
    'package/dist/docs/hub/hub-protocol.md',
    'package/dist/docs/hub/configurations.md',
    'package/dist/docs/hub/npm-release.md',
    'package/dist/docs/hub/worker-adapter.md',
    'package/dist/package.json',
    'package/dist/bin/simplex-hub.js',
    'package/dist/src/config.js',
    ...['command', 'source', 'archive', 'transaction', 'version', 'files', 'host', 'bashrc']
        .map(name => `package/dist/src/install/${name}.js`),
    'package/dist/schemas/local.jsonc',
    'package/dist/schemas/worker.yaml',
    'package/dist/web/dist/index.html',
]) {
    if (!members.includes(required)) throw new Error(`Missing npm package file: ${required}`);
}
if (!members.some(member => /^package\/dist\/web\/dist\/assets\/[^/]+\.js$/.test(member))) {
    throw new Error('The built panel JavaScript is missing from the npm package');
}
process.stdout.write(`Validated ${members.length} npm package files\n`);

// The preceding npm ci/build fills npm's cache. Install only the exact locked
// production dependencies offline beside the actual artifact, so a missing
// emitted module/dependency cannot be masked by the source tree's node_modules.
const root = mkdtempSync(join(tmpdir(), 'simplex-packed-runtime-'));
try {
    execFileSync('tar', ['-xzf', archive, '-C', root]);
    const installed = join(root, 'package');
    copyFileSync(fileURLToPath(new URL('../package-lock.json', import.meta.url)),
        join(installed, 'package-lock.json'));
    execFileSync('npm', ['ci', '--omit=dev', '--ignore-scripts', '--offline', '--no-audit', '--no-fund'],
        { cwd: installed, stdio: 'inherit', timeout: 120_000 });
    execFileSync(process.execPath, [fileURLToPath(new URL('./check-worker-installer.mjs', import.meta.url)), installed],
        { stdio: 'inherit', timeout: 60_000 });
} finally {
    rmSync(root, { recursive: true, force: true });
}
