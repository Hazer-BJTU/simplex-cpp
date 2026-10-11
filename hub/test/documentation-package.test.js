import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, readFileSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { test } from 'node:test';
import { isDocumentationAsset, stageDocumentation, validateDocumentationImages } from '../scripts/documentation.mjs';

function fixture(t) {
    const root = mkdtempSync(join(tmpdir(), 'simplex-doc-package-'));
    t.after(() => rmSync(root, { recursive: true, force: true }));
    const source = join(root, 'source');
    const target = join(root, 'package');
    function write(path, text) {
        mkdirSync(dirname(join(source, path)), { recursive: true });
        writeFileSync(join(source, path), text);
    }
    return { source, target, write };
}

test('public docs stage image bytes while excluding dependencies, output and maintainer resources', t => {
    const { source, target, write } = fixture(t);
    const image = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10, 0, 255]);
    write('hub/subagents.md', '![Status](./assets/headless-subagent.png)');
    write('hub/assets/headless-subagent.png', image);
    for (const path of ['.vitepress/dist/index.md', 'node_modules/package/index.md',
        'scripts/notes.md', 'hub/panel-redesign-plan.md', 'README.md',
        'hub/assets/local.json', 'hub/assets/code.js', 'hub/outside.png']) write(path, 'private');
    symlinkSync(join(source, 'hub/assets/headless-subagent.png'), join(source, 'hub/assets/link.png'));
    stageDocumentation(source, target);
    assert.deepEqual(readFileSync(join(target, 'hub/assets/headless-subagent.png')), image);
    assert.equal(validateDocumentationImages(target), 1);
    for (const path of ['.vitepress/dist/index.md', 'node_modules/package/index.md',
        'scripts/notes.md', 'hub/panel-redesign-plan.md', 'README.md',
        'hub/assets/local.json', 'hub/assets/code.js', 'hub/outside.png', 'hub/assets/link.png']) {
        assert.throws(() => readFileSync(join(target, path)), { code: 'ENOENT' });
    }
});

test('packaged docs reject missing images even when the source tree still has them', t => {
    const { source, target, write } = fixture(t);
    write('hub/index.md', '![Preview](assets/preview.png)');
    write('hub/assets/preview.png', 'image');
    stageDocumentation(source, target);
    rmSync(join(target, 'hub/assets/preview.png'));
    assert.throws(() => validateDocumentationImages(target), /hub\/index.md: missing packaged image/);
    assert.equal(readFileSync(join(source, 'hub/assets/preview.png'), 'utf8'), 'image');
});

test('image validation resolves reference labels, encoded names and fragments without fetching external URLs', t => {
    const { source, target, write } = fixture(t);
    write('hub/index.md', [
        '![inline](<assets/space image.svg#icon>)',
        '![reference][ Status ]',
        '![short][]',
        '[status]: assets/space%20image.svg?cache=1',
        '[short]: assets/space%20image.svg',
        '![remote](https://invalid.test/not-downloaded.png)',
        '![cdn](//invalid.test/image.png)',
        '![embedded](data:image/png;base64,AAAA)',
    ].join('\n'));
    write('hub/assets/space image.svg', '<svg/>');
    stageDocumentation(source, target);
    assert.equal(validateDocumentationImages(target), 3);
});

test('local image references cannot escape the extracted documentation directory', t => {
    const { source, target, write } = fixture(t);
    write('hub/index.md', '![Outside](../../outside.png)');
    write('hub/assets/unused.png', 'image');
    stageDocumentation(source, target);
    writeFileSync(join(target, '../outside.png'), 'exists outside docs');
    assert.throws(() => validateDocumentationImages(target), /missing packaged image/);
    assert.equal(isDocumentationAsset('hub/assets/image.png'), true);
    assert.equal(isDocumentationAsset('hub/assets/tool.js'), false);
    assert.equal(isDocumentationAsset('hub/image.png'), false);
});
