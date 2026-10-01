/** Validate rendered paths and fragments, including the GitHub project base. */
import { readdirSync, readFileSync, existsSync, statSync } from 'node:fs';
import { dirname, resolve, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parse } from 'parse5';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../.vitepress/dist');
const base = '/simplex-cpp/';
const origin = 'https://docs.invalid';
const pages = new Map();
function visit(node, result) {
    const attributes = Object.fromEntries((node.attrs ?? []).map(a => [a.name, a.value]));
    if (attributes.id) result.ids.add(attributes.id);
    for (const name of ['href', 'src']) {
        if (attributes[name]) result.links.push(attributes[name]);
    }
    for (const child of node.childNodes ?? []) visit(child, result);
}
function collect(directory) {
    for (const entry of readdirSync(directory, { withFileTypes: true })) {
        const path = resolve(directory, entry.name);
        if (entry.isDirectory()) collect(path);
        else if (entry.isFile() && path.endsWith('.html')) {
            const page = { ids: new Set(), links: [] };
            visit(parse(readFileSync(path, 'utf8')), page);
            pages.set(path, page);
        }
    }
}
collect(root);
const failures = [];
for (const [path, page] of pages) {
    const location = new URL(base + relative(root, path).split(sep).join('/'), origin);
    for (const link of page.links) {
        const url = new URL(link, location);
        if (url.origin !== origin) continue;
        if (!url.pathname.startsWith(base)) {
            failures.push(`${relative(root, path)}: outside project base: ${link}`);
            continue;
        }
        let target = resolve(root, decodeURIComponent(url.pathname.slice(base.length)));
        if (target !== root && !target.startsWith(root + sep)) {
            failures.push(`${relative(root, path)}: outside build directory: ${link}`);
            continue;
        }
        if (existsSync(target) && statSync(target).isDirectory()) target = resolve(target, 'index.html');
        if (!existsSync(target)) {
            failures.push(`${relative(root, path)}: missing target: ${link}`);
        } else if (url.hash && pages.has(target)
            && !pages.get(target).ids.has(decodeURIComponent(url.hash.slice(1)))) {
            failures.push(`${relative(root, path)}: missing anchor: ${link}`);
        }
    }
}
if (failures.length) throw new Error(failures.join('\n'));
console.log(`Validated local links, anchors, and assets across ${pages.size} rendered pages.`);
