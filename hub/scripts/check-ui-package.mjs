/** Run from an isolated, lockfile-pinned development toolchain, never a consumer's dependencies. */
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { cpSync, mkdirSync, readFileSync, readdirSync, renameSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve, relative, isAbsolute } from 'node:path';
import { fileURLToPath } from 'node:url';
import { build } from 'vite';
import { checkUiBoundary } from './check-ui-boundary.mjs';

const [archive, installed, directory] = process.argv.slice(2);
if (!archive || !installed || !directory) throw new Error('Expected archive, installed package and scratch directory');
const toolchain = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const fixtures = join(toolchain, 'fixtures');
const packageName = '@hazer-bjtu/simplex-hub';
const json = (path, value) => writeFileSync(path, `${JSON.stringify(value, null, 2)}\n`);
const env = { ...process.env };
delete env.NODE_PATH;
delete env.NODE_OPTIONS;
const execute = (args, cwd) => execFileSync(process.execPath, args, {
    cwd, env, stdio: 'inherit', timeout: 60_000,
});

for (const mode of ['installed', 'bare']) {
    const consumer = join(directory, mode);
    const scope = join(consumer, 'node_modules', '@hazer-bjtu');
    mkdirSync(scope, { recursive: true });
    const pkg = join(scope, 'simplex-hub');
    if (mode === 'installed') {
        // The existing package check unpacked this archive and installed its
        // exact locked production dependencies. Copy it without source aliases.
        cpSync(installed, pkg, { recursive: true });
    } else {
        execFileSync('tar', ['-xzf', archive, '-C', scope]);
        renameSync(join(scope, 'package'), pkg);
        assert.deepEqual(readdirSync(join(consumer, 'node_modules')), ['@hazer-bjtu']);
    }
    json(join(consumer, 'package.json'), { private: true, type: 'module' });
    const manifest = JSON.parse(readFileSync(join(pkg, 'package.json'), 'utf8'));
    assert.deepEqual(manifest.exports, {
        './ui': { types: './dist/ui/index.d.ts', import: './dist/ui/index.js' },
    });
    assert.equal(manifest.bin['simplex-hub'], './dist/bin/simplex-hub.js');
    // Do not mark CLI/server startup as globally side-effect free.
    assert.notEqual(manifest.sideEffects, false);
    for (const name of ['index', 'protocol', 'answers', 'configurations', 'rest']) {
        for (const extension of ['js', 'd.ts']) {
            assert.ok(readFileSync(join(pkg, 'dist/ui', `${name}.${extension}`)).length > 0);
        }
    }
    assert.match(readFileSync(join(pkg, 'dist/ui/answers.d.ts'), 'utf8'), /UTF-8 bytes/);
    assert.match(readFileSync(join(pkg, 'dist/ui/rest.d.ts'), 'utf8'), /Operator cleanup action/);
    const graph = checkUiBoundary(join(pkg, 'dist/ui'));
    assert.equal(graph.size, 10);
    cpSync(join(fixtures, 'import-smoke.mjs'), join(consumer, 'import-smoke.mjs'));
    execute(['import-smoke.mjs'], consumer);
    cpSync(join(fixtures, 'consumer.ts'), join(consumer, 'consumer.ts'));
    for (const resolution of ['NodeNext', 'Bundler']) {
        json(join(consumer, 'tsconfig.json'), {
            compilerOptions: {
                target: 'ES2023', lib: ['ES2023'], types: [], strict: true,
                exactOptionalPropertyTypes: true, noUncheckedIndexedAccess: true,
                skipLibCheck: false, outDir: `compiled-${resolution}`,
                module: resolution === 'NodeNext' ? 'NodeNext' : 'ESNext',
                moduleResolution: resolution,
            },
            // Include all declarations, even ones not reached by the example.
            include: ['consumer.ts', 'node_modules/@hazer-bjtu/simplex-hub/dist/ui/**/*.d.ts'],
        });
        execute([join(toolchain, 'node_modules/typescript/bin/tsc'), '-p', 'tsconfig.json'], consumer);
        if (resolution === 'NodeNext') execute(['compiled-NodeNext/consumer.js'], consumer);
    }
    cpSync(join(fixtures, 'browser.ts'), join(consumer, 'browser.ts'));
    const modules = new Set();
    await build({
        configFile: false, root: consumer, logLevel: 'warn',
        build: {
            write: false, minify: false,
            lib: { entry: join(consumer, 'browser.ts'), formats: ['es'], fileName: 'browser' },
        },
        plugins: [{
            name: 'verify-sdk-browser-graph',
            moduleParsed(info) { modules.add(info.id); },
            generateBundle(_options, bundle) {
                for (const output of Object.values(bundle)) {
                    if (output.type === 'chunk') {
                        assert.deepEqual(output.imports, [], 'browser bundle has an external dependency');
                        assert.deepEqual(output.dynamicImports, [], 'browser bundle has a dynamic dependency');
                    }
                }
            },
        }],
    });
    assert.ok(modules.size > 1, 'browser build must resolve the packed public subpath');
    for (const module of modules) {
        if (module === join(consumer, 'browser.ts')) continue;
        const rel = relative(join(pkg, 'dist/ui'), module);
        assert.ok(!rel.startsWith('..') && !isAbsolute(rel) && module.endsWith('.js'),
            `Browser dependency outside the SDK: ${module}`);
    }
    if (mode === 'installed') {
        cpSync(join(toolchain, 'examples'), join(consumer, 'examples'), { recursive: true });
        execute([join(toolchain, 'scripts/check-ui-examples.mjs'), consumer, installed], consumer);
    }
    console.log(`Packed UI SDK: ${mode} ESM, NodeNext, Bundler and browser graph passed`);
}
