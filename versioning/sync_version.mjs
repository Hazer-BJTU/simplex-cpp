#!/usr/bin/env node
/** Keep npm's required version copies aligned with the repository VERSION. */
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const mode = process.argv[2] ?? '--check';
if (!['--check', '--write'].includes(mode) || process.argv.length > 3) {
    process.stderr.write('Usage: node versioning/sync_version.mjs [--check|--write]\n');
    process.exit(2);
}

const version = readFileSync(resolve(root, 'VERSION'), 'utf8').trim();
const component = '(?:0|[1-9][0-9]*)';
const validVersion = new RegExp(`^${component}\\.${component}\\.${component}$`);
if (!validVersion.test(version)) {
    throw new Error('VERSION must contain a MAJOR.MINOR.PATCH version');
}

const manifestPath = resolve(root, 'hub/package.json');
const lockPath = resolve(root, 'hub/package-lock.json');
const manifest = JSON.parse(readFileSync(manifestPath, 'utf8'));
const lock = JSON.parse(readFileSync(lockPath, 'utf8'));
if (!lock.packages?.['']) {
    throw new Error('hub/package-lock.json has no root package entry');
}

if (mode === '--write') {
    manifest.version = version;
    lock.version = version;
    lock.packages[''].version = version;
    writeFileSync(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`);
    writeFileSync(lockPath, `${JSON.stringify(lock, null, 2)}\n`);
    process.stdout.write(`Synchronized Hub package versions to ${version}\n`);
} else {
    const mismatches = [
        ['hub/package.json', manifest.version],
        ['hub/package-lock.json', lock.version],
        ['hub/package-lock.json root package', lock.packages[''].version],
    ].filter(([, actual]) => actual !== version);
    if (mismatches.length > 0) {
        for (const [name, actual] of mismatches) {
            process.stderr.write(`${name}: expected ${version}, found ${String(actual)}\n`);
        }
        process.stderr.write('Run npm run version:sync from hub/ after editing VERSION.\n');
        process.exitCode = 1;
    } else {
        process.stdout.write(`Project version ${version} is synchronized\n`);
    }
}
