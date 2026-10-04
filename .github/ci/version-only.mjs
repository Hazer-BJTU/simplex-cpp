/** Content verification for the narrowly scoped ordinary-CI version bypass. */
import { execFileSync } from 'node:child_process';

const versionFiles = ['VERSION', 'hub/package.json', 'hub/package-lock.json'];
const versionPattern = /^(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)$/;
const maxBlobBytes = 16 * 1024 * 1024;

export function isProjectVersion(value) {
    return typeof value === 'string' && value === value.trim() && versionPattern.test(value);
}

function git(cwd, args) {
    return execFileSync('git', args, {
        cwd, encoding: 'buffer', maxBuffer: maxBlobBytes,
        stdio: ['ignore', 'pipe', 'pipe'],
    });
}

function decode(buffer) {
    // Preserve a BOM: JSON does not allow one, so do not silently normalize it.
    return new TextDecoder('utf-8', { fatal: true, ignoreBOM: true }).decode(buffer);
}

/**
 * Parse JSON into a lossless comparison tree. Object order and insignificant
 * whitespace are ignored; duplicate decoded keys are rejected at every depth.
 * Number tokens retain their spelling, avoiding IEEE-754 rounding that could
 * otherwise hide changes to large integers. No manifest code is executed.
 */
export function parseComparisonJson(text) {
    let offset = 0;
    const whitespace = /[ \t\r\n]*/y;
    const string = /"(?:[^"\\\u0000-\u001f]|\\(?:["\\/bfnrt]|u[0-9a-fA-F]{4}))*"/y;
    const scalar = /(?:null|true|false|-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)/y;

    function skipWhitespace() {
        whitespace.lastIndex = offset;
        offset += whitespace.exec(text)[0].length;
    }

    function readString() {
        string.lastIndex = offset;
        const match = string.exec(text);
        if (!match) {
            throw new Error('invalid JSON string in version metadata');
        }
        offset += match[0].length;
        return JSON.parse(match[0]);
    }

    function readValue(depth) {
        if (depth > 128) {
            throw new Error('version metadata exceeds the JSON depth limit');
        }
        skipWhitespace();
        const character = text[offset];
        if (character === '"') {
            return { kind: 'string', value: readString() };
        }
        if (character === '{' || character === '[') {
            const object = character === '{';
            const end = object ? '}' : ']';
            const value = object ? new Map() : [];
            offset++;
            skipWhitespace();
            if (text[offset] !== end) {
                for (;;) {
                    skipWhitespace();
                    let key;
                    if (object) {
                        key = readString();
                        if (value.has(key)) {
                            throw new Error('duplicate JSON key in version metadata');
                        }
                        skipWhitespace();
                        if (text[offset++] !== ':') {
                            throw new Error('missing JSON object separator');
                        }
                    }
                    const item = readValue(depth + 1);
                    if (object) {
                        value.set(key, item);
                    } else {
                        value.push(item);
                    }
                    skipWhitespace();
                    if (text[offset] !== ',') {
                        break;
                    }
                    offset++;
                }
            }
            if (text[offset++] !== end) {
                throw new Error('invalid JSON container in version metadata');
            }
            return { kind: object ? 'object' : 'array', value };
        }
        scalar.lastIndex = offset;
        const match = scalar.exec(text);
        if (!match) {
            throw new Error('invalid JSON value in version metadata');
        }
        offset += match[0].length;
        return { kind: 'scalar', value: match[0] };
    }

    const value = readValue(0);
    skipWhitespace();
    if (offset !== text.length) {
        throw new Error('trailing data in version metadata');
    }
    return value;
}

/** Canonicalize the comparison tree without converting number tokens to Number. */
function canonical(node) {
    if (node.kind === 'object') {
        return ['object', [...node.value].sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0)
            .map(([key, value]) => [key, canonical(value)])];
    }
    if (node.kind === 'array') {
        return ['array', node.value.map(canonical)];
    }
    return [node.kind, node.value];
}

function field(node, key) {
    return node?.kind === 'object' ? node.value.get(key) : undefined;
}

/** Validate all version copies before masking only the three allowed JSON values. */
export function verifyVersionContents(before, after) {
    const from = before.VERSION.trim();
    const to = after.VERSION.trim();
    if (!isProjectVersion(from) || !isProjectVersion(to) || from === to) {
        return null;
    }
    for (const path of versionFiles.slice(1)) {
        const oldTree = parseComparisonJson(before[path]);
        const newTree = parseComparisonJson(after[path]);
        for (const [tree, version] of [[oldTree, from], [newTree, to]]) {
            const copies = [field(tree, 'version')];
            if (path === 'hub/package-lock.json') {
                copies.push(field(field(field(tree, 'packages'), ''), 'version'));
            }
            if (copies.some((copy) => copy?.kind !== 'string' || copy.value !== version)) {
                return null;
            }
            for (const copy of copies) {
                copy.value = ''; // Owned parse trees only; never modifies input files.
            }
        }
        if (JSON.stringify(canonical(oldTree)) !== JSON.stringify(canonical(newTree))) {
            return null;
        }
    }
    return { from, to };
}

/**
 * Verify the complete resolved range, using raw Git modes/statuses and blobs.
 * A false predicate returns null so normal path rules still apply. Unreadable or
 * ambiguous data throws so the classifier selects its full-validation fallback.
 * Files must be modified regular files, with the same mode on both sides.
 */
export function verifyVersionOnlyChange(cwd, range, paths) {
    if (paths.length !== versionFiles.length
        || !versionFiles.every((path) => paths.includes(path))) {
        return null;
    }
    const raw = decode(git(cwd, [
        'diff', '--no-ext-diff', '--no-textconv', '--raw', '--no-abbrev', '-z',
        '--find-renames', range.base, range.head, '--',
    ]));
    if (!raw.endsWith('\0')) {
        throw new Error('unterminated Git raw diff');
    }
    const records = raw.slice(0, -1).split('\0');
    const before = {};
    const after = {};
    // Any rename/copy has a non-M header and is rejected before interpreting its
    // extra path. This also rejects additions, deletions and type/mode changes.
    for (let index = 0; index < records.length; index += 2) {
        const match = /^:(100644|100755) \1 ([0-9a-f]{40}) ([0-9a-f]{40}) M$/.exec(records[index]);
        const path = records[index + 1];
        if (!match || !versionFiles.includes(path) || Object.hasOwn(before, path)) {
            return null;
        }
        before[path] = decode(git(cwd, ['cat-file', 'blob', match[2]]));
        after[path] = decode(git(cwd, ['cat-file', 'blob', match[3]]));
    }
    if (Object.keys(before).length !== versionFiles.length) {
        return null;
    }
    return verifyVersionContents(before, after);
}
