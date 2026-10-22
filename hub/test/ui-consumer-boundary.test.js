/** Public contract imports use the same barrel as external clients; runtime helpers stay private. */
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { test } from 'node:test';
import { parse } from '@babel/parser';

const root = fileURLToPath(new URL('../', import.meta.url));
const helpers = new Map([
    ['background.ts', new Set(['normalizeBackground'])],
    ['guards.ts', new Set(['checkHubEnvelope'])],
    ['answers.ts', new Set(['answerSource', 'answerPage'])],
    ['tool-batches.ts', new Set(['omittedToolItems', 'toolOmissionCount'])],
]);
test('panel contracts go through ui/index; only explicit runtime helpers may use shared', () => {
    function walk(directory) {
        for (const entry of readdirSync(directory, { withFileTypes: true })) {
            const path = join(directory, entry.name);
            if (entry.isDirectory()) { walk(path); continue; }
            if (!/\.tsx?$/.test(path)) continue;
            const ast = parse(readFileSync(path, 'utf8'), { sourceType: 'module', plugins: ['typescript', 'jsx'] });
            function inspect(node) {
                if (!node || typeof node !== 'object') return;
                const source = node.source;
                if (source?.type === 'StringLiteral') {
                    const target = resolve(dirname(path), source.value);
                    assert.ok(!target.startsWith(join(root, 'src') + '/'), `server import in ${path}`);
                    if (target.startsWith(join(root, 'ui') + '/')) assert.equal(target, join(root, 'ui/index.ts'));
                    if (target.startsWith(join(root, 'shared') + '/')) {
                        const allowed = helpers.get(target.slice(target.lastIndexOf('/') + 1));
                        assert.ok(allowed && node.type === 'ImportDeclaration' && node.importKind !== 'type', `private contract import in ${path}`);
                        for (const specifier of node.specifiers) assert.ok(specifier.importKind !== 'type'
                            && allowed.has(specifier.imported?.name), `unexpected shared export in ${path}`);
                    }
                }
                for (const [key, value] of Object.entries(node)) {
                    if (key === 'loc') continue;
                    if (Array.isArray(value)) value.forEach(inspect);
                    else if (value && typeof value === 'object') inspect(value);
                }
            }
            inspect(ast.program);
        }
    }
    walk(join(root, 'web/src'));
});
