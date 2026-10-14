/** TypeScript rewrites JS specifiers but retains .ts paths in emitted declarations. */
import { readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const directory = fileURLToPath(new URL('../dist/ui', import.meta.url));
for (const name of readdirSync(directory)) {
    if (!name.endsWith('.d.ts')) continue;
    const path = join(directory, name);
    const source = readFileSync(path, 'utf8');
    // Only relative import/export and import-type specifiers emitted by tsc.
    // Declarations reference the same .js paths as runtime; TypeScript resolves
    // those to sibling .d.ts files, without needing source .ts files in the pack.
    const output = source.replace(/(\bfrom\s+|\bimport\s*\(\s*)(['"])(\.{1,2}\/[^'"\n]+)\.ts\2/g,
        '$1$2$3.js$2');
    writeFileSync(path, output);
}
