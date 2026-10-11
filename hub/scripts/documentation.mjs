/** Public documentation resources shared by npm staging and artifact validation. */
import { cpSync, existsSync, mkdirSync, readFileSync, readdirSync, statSync } from 'node:fs';
import { dirname, relative, resolve, sep } from 'node:path';

const IMAGE_EXTENSION = /\.(?:png|jpe?g|gif|svg|webp|avif)$/i;

/** Only images inside a named assets directory accompany the public Markdown. */
export function isDocumentationAsset(path) {
    return path.split('/').includes('assets') && IMAGE_EXTENSION.test(path);
}

/** Do not copy site dependencies, generated output, symlinks or maintainer notes. */
export function stageDocumentation(source, destination, path = '') {
    mkdirSync(destination, { recursive: true });
    for (const entry of readdirSync(source, { withFileTypes: true })) {
        if (entry.name.startsWith('.') || entry.name === 'node_modules'
            || entry.name === 'scripts' || entry.name === 'panel-redesign-plan.md'
            || entry.name === 'README.md') continue;
        const child = path ? `${path}/${entry.name}` : entry.name;
        const from = resolve(source, entry.name);
        const to = resolve(destination, entry.name);
        if (entry.isDirectory()) stageDocumentation(from, to, child);
        else if (entry.isFile() && (entry.name.endsWith('.md') || isDocumentationAsset(child))) {
            cpSync(from, to);
        }
    }
}

/** Inline and reference-style Markdown images; labels are case-insensitive. */
function imageTargets(markdown) {
    const targets = [];
    const references = new Map();
    const label = value => value.trim().replace(/\s+/g, ' ').toLowerCase();
    for (const match of markdown.matchAll(/^ {0,3}\[([^\]]+)\]:\s*(?:<([^>]+)>|(\S+))/gm)) {
        references.set(label(match[1]), match[2] ?? match[3]);
    }
    for (const match of markdown.matchAll(/!\[[^\]]*\]\(\s*(?:<([^>]+)>|([^\s)]+))[^)]*\)/g)) {
        targets.push(match[1] ?? match[2]);
    }
    for (const match of markdown.matchAll(/!\[([^\]]*)\](?:\[([^\]]*)\])?(?!\()/g)) {
        const target = references.get(label(match[2] || match[1]));
        if (target) targets.push(target);
    }
    return targets;
}

/** Check the extracted artifact, so source-tree assets cannot mask missing files. */
export function validateDocumentationImages(root) {
    const docs = resolve(root);
    let images = 0;
    function visit(directory) {
        for (const entry of readdirSync(directory, { withFileTypes: true })) {
            const path = resolve(directory, entry.name);
            if (entry.isDirectory()) visit(path);
            else if (entry.isFile() && entry.name.endsWith('.md')) {
                for (const target of imageTargets(readFileSync(path, 'utf8'))) {
                    if (/^(?:[a-z][a-z\d+.-]*:|\/\/|#)/i.test(target)) continue;
                    const file = decodeURIComponent(target.split(/[?#]/, 1)[0]);
                    const resolved = resolve(dirname(path), file);
                    if (!resolved.startsWith(docs + sep) || !existsSync(resolved)
                        || !statSync(resolved).isFile()) {
                        throw new Error(`${relative(docs, path)}: missing packaged image: ${target}`);
                    }
                    images += 1;
                }
            }
        }
    }
    visit(docs);
    return images;
}
