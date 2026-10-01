#!/usr/bin/env node
/** Put runtime resources beside the emitted server tree. */
import { chmodSync, cpSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
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
// Stage public Markdown only: site dependencies, build output, and historical
// notes must never enter the runtime npm package.
const docs = resolve(dist, 'docs');
rmSync(docs, { recursive: true, force: true });
function stageDocs(source, destination) {
    mkdirSync(destination, { recursive: true });
    for (const entry of readdirSync(source, { withFileTypes: true })) {
        if (entry.name.startsWith('.') || entry.name === 'node_modules'
            || entry.name === 'scripts' || entry.name === 'panel-redesign-plan.md'
            || entry.name === 'README.md') continue;
        const from = resolve(source, entry.name);
        const to = resolve(destination, entry.name);
        if (entry.isDirectory()) stageDocs(from, to);
        else if (entry.isFile() && entry.name.endsWith('.md')) cpSync(from, to);
    }
}
stageDocs(resolve(root, '..', 'docs'), docs);
const { name, version } = JSON.parse(readFileSync(resolve(root, 'package.json'), 'utf8'));
writeFileSync(resolve(dist, 'package.json'), `${JSON.stringify({ name, version, type: 'module' }, null, 2)}\n`);
