/** Complete-tree replacement with rollback and recovery under a destination lock. */
import { createHash } from 'node:crypto';
import { chmod, lstat, mkdir, readdir, readFile, realpath, rename, rm } from 'node:fs/promises';
import { basename, dirname, join, resolve, relative, sep } from 'node:path';
import { homedir } from 'node:os';
import { acquireLock, atomicWrite, exists, syncDirectory } from './files.ts';

/** Stop before creating staging files for dangerous or symlinked destinations. */
export async function destinationPath(input: string, home = homedir()): Promise<string> {
    if (!input || /[\x00-\x1f\x7f]/.test(input)) throw new Error('Installation directory must be a non-empty path without control characters');
    const requested = resolve(input);
    // Resolve existing ancestors, even when the final parent does not yet exist.
    let ancestor = requested;
    const missing: string[] = [];
    while (!(await exists(ancestor))) {
        missing.unshift(basename(ancestor));
        ancestor = dirname(ancestor);
    }
    const canonical = resolve(await realpath(ancestor), ...missing);
    if (await exists(requested) && (await lstat(requested)).isSymbolicLink()) {
        throw new Error('Installation destination must not be a symlink');
    }
    const actualHome = await realpath(home);
    const within = (child: string, parent: string) => {
        const path = relative(parent, child);
        return !path || (!path.startsWith(`..${sep}`) && path !== '..' && !path.startsWith(sep));
    };
    const protectedPaths = ['/', '/bin', '/sbin', '/lib', '/lib64', '/usr', '/usr/bin', '/usr/sbin',
        '/usr/lib', '/usr/lib64', '/usr/libexec', '/usr/share', '/usr/local', '/usr/local/bin',
        '/usr/local/sbin', '/usr/local/lib', '/usr/local/share', '/etc', '/var', '/opt', '/tmp',
        '/home', '/root', '/srv', '/dev', '/proc', '/sys', '/run'];
    if (protectedPaths.includes(canonical) || within(actualHome, canonical) || within(await realpath(process.cwd()), canonical)) {
        throw new Error(`Refusing dangerous installation target: ${canonical}`);
    }
    if (await exists(canonical) && !(await lstat(canonical)).isDirectory()) {
        throw new Error('Installation destination exists and is not a directory');
    }
    return canonical;
}

export class InstallationTransaction {
    readonly destination: string;
    readonly work: string;
    readonly prepared: string;
    private readonly backup: string;
    private readonly journal: string;
    private releaseLock: (() => Promise<void>) | undefined;

    constructor(destination: string) {
        this.destination = destination;
        const key = createHash('sha256').update(destination).digest('hex').slice(0, 16);
        this.work = join(dirname(destination), `.simplex-install-${key}`);
        this.prepared = join(this.work, 'prepared');
        this.backup = join(this.work, 'previous');
        this.journal = join(this.work, 'transaction.json');
    }

    /** Take a kernel lock, recover the previous transaction, then clear abandoned preparation. */
    async open(): Promise<void> {
        await mkdir(dirname(this.destination), { recursive: true });
        await mkdir(this.work, { mode: 0o700 }).catch((error: NodeJS.ErrnoException) => {
            if (error.code !== 'EEXIST') throw error;
        });
        const stat = await lstat(this.work);
        if (!stat.isDirectory() || (process.getuid && stat.uid !== process.getuid()) || (stat.mode & 0o022)) {
            throw new Error(`Installer workspace must be a private owned directory: ${this.work}`);
        }
        await chmod(this.work, 0o700);
        this.releaseLock = await acquireLock(join(this.work, 'lock'));
        try {
            await this.recover();
            for (const entry of await readdir(this.work)) {
                if (entry !== 'lock') await rm(join(this.work, entry), { recursive: true, force: true });
            }
        } catch (error) {
            await this.close();
            throw error;
        }
    }

    /**
     * A durable journal means publication was not committed. Restore the old
     * tree if present. Without a journal, an orphan backup is only old garbage
     * from an already committed publication and must not replace the live tree.
     */
    async recover(): Promise<void> {
        if (!(await exists(this.journal))) return;
        const stat = await lstat(this.journal);
        if (!stat.isFile() || stat.size > 1024) throw new Error('Invalid installation recovery journal');
        const record = JSON.parse(await readFile(this.journal, 'utf8')) as { hadDestination: boolean };
        if (typeof record.hadDestination !== 'boolean') throw new Error('Invalid installation recovery journal');
        if (await exists(this.backup)) {
            if (!(await lstat(this.backup)).isDirectory()) throw new Error('Invalid installation backup');
            await rm(this.destination, { recursive: true, force: true });
            await rename(this.backup, this.destination);
            await syncDirectory(dirname(this.destination));
        } else if (!record.hadDestination && !(await exists(this.prepared))) {
            // Fresh installation was published but never committed.
            await rm(this.destination, { recursive: true, force: true });
            await syncDirectory(dirname(this.destination));
        } else if (record.hadDestination && !(await exists(this.destination))) {
            throw new Error(`Cannot recover installation automatically; inspect ${this.work}`);
        }
        await rm(this.journal);
        await syncDirectory(this.work);
    }

    /** Publish a verified tree; the callback is a narrow fault-injection seam for tests. */
    async replace(tree: string, beforePublish?: () => Promise<void>): Promise<void> {
        await rename(tree, this.prepared);
        const hadDestination = await exists(this.destination);
        await atomicWrite(this.journal, `${JSON.stringify({ hadDestination })}\n`);
        try {
            if (hadDestination) await rename(this.destination, this.backup);
            await syncDirectory(dirname(this.destination));
            await syncDirectory(this.work);
            await beforePublish?.();
            await rename(this.prepared, this.destination);
            await syncDirectory(dirname(this.destination));
            await syncDirectory(this.work);
            // Removing the journal commits publication. Orphan backup cleanup is
            // intentionally left to close()/the next lock holder after this point.
            await rm(this.journal);
            await syncDirectory(this.work);
        } catch (error) {
            try { await this.recover(); }
            catch (rollback) {
                throw new Error(`Installation failed: ${(error as Error).message}; rollback failed: ${(rollback as Error).message}; recovery files retained in ${this.work}`);
            }
            throw error;
        }
    }

    /** Preserve recovery files on rollback failure; release the kernel lock in all cases. */
    async close(): Promise<void> {
        if (!this.releaseLock) return;
        try {
            if (!(await exists(this.journal))) {
                for (const entry of await readdir(this.work)) {
                    if (entry !== 'lock') await rm(join(this.work, entry), { recursive: true, force: true });
                }
            }
        } finally {
            const release = this.releaseLock;
            this.releaseLock = undefined;
            await release();
        }
    }
}
