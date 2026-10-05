/** Prepare an immutable archive in a private staging area before touching the live tree. */
import { createHash } from 'node:crypto';
import { createReadStream, createWriteStream } from 'node:fs';
import { mkdir, open, stat } from 'node:fs/promises';
import { join, posix } from 'node:path';
import { Transform, Readable } from 'node:stream';
import { pipeline } from 'node:stream/promises';
import { createGunzip } from 'node:zlib';
import * as tar from 'tar';
import { fetchResponse, readBounded } from './source.ts';
import type { Fetch, WorkerRelease } from './source.ts';
import { METADATA, validateTree } from './version.ts';

const MAX_DOWNLOAD = 256 * 1024 * 1024;
const MAX_EXPANDED = 2 * 1024 * 1024 * 1024;

/** Bound disk consumption even when Content-Length is absent or misleading. */
function byteLimit(limit: number): Transform {
    let size = 0;
    return new Transform({
        transform(chunk: Buffer, _encoding, callback) {
            size += chunk.length;
            callback(size > limit ? new Error('Worker archive exceeds the size limit') : null, chunk);
        },
    });
}

interface Member { type: string; target?: string }

/**
 * Inspect the whole archive before extraction. No entry may escape its release
 * root, overwrite another entry, or write through a link. Library symlink chains
 * are allowed only when they resolve to an archived regular file inside the root.
 */
export async function validateArchive(file: string, root: string): Promise<void> {
    const size = (await stat(file)).size;
    if (size < 1024 || size % 512 !== 0) throw new Error('Truncated or malformed tar archive');
    const handle = await open(file, 'r');
    try {
        const tail = Buffer.alloc(1024);
        await handle.read(tail, 0, tail.length, size - tail.length);
        if (tail.some(byte => byte !== 0)) throw new Error('Tar archive is missing its end marker');
    } finally {
        await handle.close();
    }
    const members = new Map<string, Member>();
    let failure: Error | undefined;
    await tar.t({ file, strict: true, onReadEntry(entry) {
        try {
            const path = entry.path.replace(/\/$/, '');
            const segments = path.split('/');
            if (!path || segments[0] !== root || segments.some(part => !part || part === '.' || part === '..')
                || /[\\\x00-\x1f\x7f]/.test(path)) throw new Error(`Unsafe archive path: ${entry.path}`);
            if (members.has(path) || members.size >= 100_000) throw new Error('Duplicate or excessive archive entries');
            if (!['File', 'Directory', 'SymbolicLink', 'Link'].includes(entry.type)
                || ((entry.mode ?? 0) & 0o7000) !== 0) throw new Error(`Unsafe archive entry type/mode: ${path}`);
            if (path === `${root}/${METADATA}`) throw new Error('Archive contains reserved installer metadata');
            const member: Member = { type: entry.type };
            if (entry.type === 'SymbolicLink' || entry.type === 'Link') {
                const link = entry.linkpath ?? '';
                if (!link || posix.isAbsolute(link) || /[\\\x00-\x1f\x7f]/.test(link)) {
                    throw new Error(`Unsafe archive link: ${path}`);
                }
                member.target = posix.normalize(entry.type === 'Link' ? link : posix.join(posix.dirname(path), link));
                if (!member.target.startsWith(`${root}/`)) throw new Error(`Archive link escapes release root: ${path}`);
            }
            members.set(path, member);
        } catch (error) {
            failure ??= error as Error;
        }
    } });
    if (failure) throw failure;
    if (members.get(root)?.type !== 'Directory') throw new Error('Archive is missing the release root directory');
    for (const [path, member] of members) {
        let parent = posix.dirname(path);
        while (parent !== '.' && parent !== root) {
            const ancestor = members.get(parent);
            if (ancestor && ancestor.type !== 'Directory') throw new Error(`Archive writes through a non-directory: ${parent}`);
            parent = posix.dirname(parent);
        }
        if (member.target) {
            const visited = new Set([path]);
            let target: string | undefined = member.target;
            while (target) {
                if (visited.has(target)) throw new Error(`Cyclic archive link: ${path}`);
                visited.add(target);
                const next = members.get(target);
                if (!next || next.type === 'Directory') throw new Error(`Archive link does not resolve to a regular file: ${path}`);
                target = next.target;
            }
        }
    }
}

/** Download, checksum, decompress with a bound, inspect, then extract the entire tree. */
export async function prepareArchive(release: WorkerRelease, work: string,
    fetcher: Fetch = fetch, signal?: AbortSignal): Promise<string> {
    const operation = signal ? AbortSignal.any([signal, AbortSignal.timeout(300_000)]) : AbortSignal.timeout(300_000);
    const checksums = (await readBounded(await fetchResponse(release.checksumUrl, fetcher, operation), 1024 * 1024))
        .toString('utf8').split(/\r?\n/);
    const hashes = checksums.flatMap(line => {
        const match = /^([a-fA-F0-9]{64}) [ *](.+)$/.exec(line);
        return match?.[2] === release.archive ? [match[1]!.toLowerCase()] : [];
    });
    if (hashes.length !== 1) throw new Error(`SHA256SUMS must contain exactly one checksum for ${release.archive}`);
    const compressed = join(work, 'download.tar.gz');
    const response = await fetchResponse(release.archiveUrl, fetcher, operation);
    if (!response.body) throw new Error('Worker archive download has no body');
    const hash = createHash('sha256');
    const hashing = new Transform({ transform(chunk: Buffer, _encoding, done) { hash.update(chunk); done(null, chunk); } });
    await pipeline(Readable.fromWeb(response.body as Parameters<typeof Readable.fromWeb>[0]),
        byteLimit(MAX_DOWNLOAD), hashing, createWriteStream(compressed, { flags: 'wx', mode: 0o600 }), { signal: operation });
    if (hash.digest('hex') !== hashes[0]) throw new Error('Worker archive SHA256 checksum mismatch');
    const uncompressed = join(work, 'download.tar');
    await pipeline(createReadStream(compressed), createGunzip(), byteLimit(MAX_EXPANDED),
        createWriteStream(uncompressed, { flags: 'wx', mode: 0o600 }), { signal: operation });
    await validateArchive(uncompressed, release.root);
    const unpack = join(work, 'unpack');
    await mkdir(unpack, { mode: 0o700 });
    await tar.x({ file: uncompressed, cwd: unpack, strict: true, preserveOwner: false,
        chmod: true, noMtime: true });
    operation.throwIfAborted();
    const tree = join(unpack, release.root);
    await validateTree(tree);
    return tree;
}
