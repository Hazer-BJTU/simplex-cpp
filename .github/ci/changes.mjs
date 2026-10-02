/** Event ranges and complete, NUL-delimited Git diffs, independent of path rules. */
import { execFileSync } from 'node:child_process';

function git(cwd, args, encoding = 'utf8') {
    return execFileSync('git', args, {
        cwd, encoding, maxBuffer: 64 * 1024 * 1024, stdio: ['ignore', 'pipe', 'pipe'],
    });
}

function commit(cwd, sha) {
    if (typeof sha !== 'string' || !/^[0-9a-f]{40}$/.test(sha) || /^0+$/.test(sha)) {
        throw new Error('missing, zero or invalid event commit SHA');
    }
    return git(cwd, ['rev-parse', '--verify', `${sha}^{commit}`]).trim();
}

/**
 * PR checkout is the merge revision, not just the most recent head commit.
 * A complete checkout (fetch-depth: 0) normally supplies both event commits.
 * Unavailable/unsupported ranges throw; the entry point falls back to native.
 */
export function resolveRange(cwd, eventName, event, revision) {
    const checkedOut = git(cwd, ['rev-parse', 'HEAD']).trim();
    const expected = commit(cwd, revision);
    if (checkedOut !== expected) {
        throw new Error('checkout does not match the workflow event revision');
    }
    if (eventName === 'pull_request') {
        const base = commit(cwd, event.pull_request?.base?.sha);
        const head = commit(cwd, event.pull_request?.head?.sha);
        git(cwd, ['merge-base', '--is-ancestor', head, checkedOut]);
        const mergeBase = git(cwd, ['merge-base', base, checkedOut]).trim();
        return { base: mergeBase, head: checkedOut };
    }
    if (eventName === 'push' && event.ref === 'refs/heads/main') {
        const base = commit(cwd, event.before);
        const head = commit(cwd, event.after);
        if (head !== checkedOut) {
            throw new Error('push after SHA does not match checkout');
        }
        return { base, head };
    }
    throw new Error(`unsupported CI event: ${eventName}`);
}

/** Keep both rename/copy paths; filenames may contain tabs and newlines. */
export function parseChangedPaths(buffer) {
    const data = new TextDecoder('utf-8', { fatal: true }).decode(buffer);
    if (data === '') {
        return [];
    }
    if (!data.endsWith('\0')) {
        throw new Error('unterminated Git name-status output');
    }
    const fields = data.slice(0, -1).split('\0');
    const paths = [];
    for (let index = 0; index < fields.length;) {
        const status = fields[index++];
        // Git prints a three-digit similarity for renames/copies (R100, R080,
        // C090). Accept R000-R100/C000-C100 explicitly; anything else is
        // malformed and must fail closed rather than guess at a path count.
        const similarity = /^[RC](\d{3})$/.exec(status);
        const renameOrCopy = similarity !== null && Number(similarity[1]) <= 100;
        const count = renameOrCopy ? 2
            : /^[AMDT]$/.test(status) ? 1 : 0;
        if (!count || index + count > fields.length) {
            throw new Error('malformed or unsupported Git name-status record');
        }
        for (let remaining = count; remaining > 0; remaining--) {
            const path = fields[index++];
            if (!path) {
                throw new Error('empty Git changed path');
            }
            paths.push(path);
        }
    }
    return [...new Set(paths)];
}

export function collectChangedPaths(cwd, range) {
    return parseChangedPaths(git(cwd, [
        'diff', '--no-ext-diff', '--no-textconv', '--name-status', '-z',
        '--find-renames', range.base, range.head, '--',
    ], 'buffer'));
}
