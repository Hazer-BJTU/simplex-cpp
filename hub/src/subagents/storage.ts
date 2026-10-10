/** Private, bounded, atomic files for subagent ownership and operation receipts. */
import { closeSync, constants, existsSync, fstatSync, fsyncSync, lstatSync, mkdirSync,
    openSync, readSync, readdirSync, realpathSync, renameSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { randomUUID } from 'node:crypto';
import { sessionDir } from '../launch/config-render.ts';
import { isSubagentId, validateSessionId } from '../state/session-id.ts';

/** Resolve operator-selected root/ancestor links once, before deriving managed paths. */
export function canonicalDataRoot(directory: string): string {
    // recursive mkdir supports a missing suffix below an existing linked ancestor.
    // A dangling link or non-directory still fails, instead of becoming a new root.
    mkdirSync(resolve(directory), { recursive: true, mode: 0o700 });
    const root = realpathSync(directory);
    if (!lstatSync(root).isDirectory()) throw new Error('data root is not a directory');
    return root;
}

/** Refuse directory links in managed paths. DataDir was canonicalized at assembly. */
export function privateDirectory(directory: string, create = false): void {
    const absolute = resolve(directory);
    const parent = dirname(absolute);
    if (parent !== absolute) privateDirectory(parent, create);
    if (!existsSync(absolute)) {
        // lstat also notices dangling symlinks which existsSync deliberately hides.
        try { lstatSync(absolute); throw new Error('unsafe directory'); }
        catch (error) { if ((error as NodeJS.ErrnoException).code !== 'ENOENT') throw error; }
        if (create) mkdirSync(absolute, { mode: 0o700 });
        return;
    }
    const stat = lstatSync(absolute);
    if (!stat.isDirectory() || stat.isSymbolicLink()) throw new Error('unsafe directory');
}

/** All loads are size-bounded and reject symlinks/special files before reading. */
export function readPrivate(path: string, maxBytes: number): unknown | null {
    privateDirectory(dirname(path));
    let fd: number;
    try { fd = openSync(path, constants.O_RDONLY | constants.O_NOFOLLOW | constants.O_NONBLOCK); }
    catch (error) { if ((error as NodeJS.ErrnoException).code === 'ENOENT') return null; throw error; }
    try {
        const stat = fstatSync(fd);
        if (!stat.isFile() || stat.size > maxBytes) throw new Error('invalid private state file');
        // Bound allocation and reads even if another process grows the file.
        const bytes = Buffer.alloc(maxBytes + 1);
        let length = 0;
        while (length < bytes.length) {
            const count = readSync(fd, bytes, length, bytes.length - length, null);
            if (!count) break;
            length += count;
        }
        if (length > maxBytes) throw new Error('private state exceeds byte limit');
        return JSON.parse(bytes.subarray(0, length).toString('utf8')) as unknown;
    } finally { closeSync(fd); }
}

/** fsync the contents before rename. A failed publication preserves the old file. */
export function writePrivate(path: string, value: unknown, maxBytes: number): void {
    const bytes = JSON.stringify(value) + '\n';
    if (Buffer.byteLength(bytes) > maxBytes) throw new Error('private state exceeds byte limit');
    privateDirectory(dirname(path), true);
    const temporary = `${path}.${randomUUID()}.tmp`;
    try {
        const fd = openSync(temporary, 'wx', 0o600);
        try { writeFileSync(fd, bytes); fsyncSync(fd); }
        finally { closeSync(fd); }
        renameSync(temporary, path);
    } finally { rmSync(temporary, { force: true }); }
}

/** Enumerate only canonical generated IDs, never traverse a linked root. */
export function childDirectories(dataDir: string): string[] {
    const root = join(dataDir, 'subagents');
    privateDirectory(root);
    if (!existsSync(root)) return [];
    return readdirSync(root).filter(isSubagentId);
}

/** rm does not follow interior symlinks; the owned root must be an actual directory. */
export function removeChildDirectory(dataDir: string, id: string): void {
    if (!isSubagentId(id)) throw new Error('not a generated subagent ID');
    const root = sessionDir({ dataDir }, id);
    privateDirectory(root);
    rmSync(root, { recursive: true, force: true });
}

export function ownedPath(dataDir: string, id: string, file: string): string {
    validateSessionId(id);
    const root = sessionDir({ dataDir }, id);
    privateDirectory(root);
    return join(root, file);
}
