#!/usr/bin/env node
/** Put runtime resources beside the emitted server tree. */
import { chmodSync, cpSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const dist = resolve(root, 'dist');
if (process.argv[2] === '--prepare') {
    rmSync(dist, { recursive: true, force: true });
    process.exit(0);
}
if (process.argv.length !== 2) {
    throw new Error('Usage: stage-release.mjs [--prepare]');
}
mkdirSync(dist, { recursive: true });
chmodSync(resolve(dist, 'bin', 'simplex-hub.js'), 0o755);
for (const resource of ['schemas', 'web/dist']) {
    const destination = resolve(dist, resource);
    rmSync(destination, { recursive: true, force: true });
    cpSync(resolve(root, resource), destination, { recursive: true });
}
// Documentation sources live at the repository root; only public references
// are staged into the npm artifact, alongside the compiled application.
const docs = resolve(dist, 'docs');
rmSync(docs, { recursive: true, force: true });
for (const section of ['core', 'hub']) {
    mkdirSync(resolve(docs, section), { recursive: true });
}
for (const file of ['index.md', 'worker-protocol.md']) {
    const source = resolve(root, '..', 'docs', 'core', file);
    const content = readFileSync(source, 'utf8').replaceAll(
        '../../load/README.md',
        'https://github.com/Hazer-BJTU/simplex-cpp/blob/main/load/README.md');
    writeFileSync(resolve(docs, 'core', file), content);
}
for (const file of ['configurations.md', 'hub-protocol.md', 'npm-release.md', 'worker-adapter.md']) {
    const source = resolve(root, '..', 'docs', 'hub', file);
    const content = readFileSync(source, 'utf8').replaceAll('../../hub/schemas/', '../../schemas/');
    writeFileSync(resolve(docs, 'hub', file), content);
}
const { name, version } = JSON.parse(readFileSync(resolve(root, 'package.json'), 'utf8'));
writeFileSync(resolve(dist, 'package.json'), `${JSON.stringify({ name, version, type: 'module' }, null, 2)}\n`);
