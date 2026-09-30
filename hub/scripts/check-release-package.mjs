#!/usr/bin/env node
/** Verify the packed artifact rather than trusting the working tree layout. */
import { execFileSync } from 'node:child_process';
import { resolve } from 'node:path';

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
    'package/docs/',
];
for (const member of members) {
    if (!allowed.some(prefix => member === prefix ||
        (prefix.endsWith('/') && member.startsWith(prefix)))) {
        throw new Error(`Unexpected file in npm package: ${member}`);
    }
}
for (const required of [
    'package/LICENSE',
    'package/dist/package.json',
    'package/dist/bin/simplex-hub.js',
    'package/dist/src/config.js',
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
