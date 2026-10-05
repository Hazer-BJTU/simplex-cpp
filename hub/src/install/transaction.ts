/** Complete-tree replacement with rollback and recovery under a destination lock. */
import { createHash, randomUUID } from 'node:crypto';
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
    /** Unique per invocation: obsolete, undeletable trees cannot occupy the next staging/backup slots. */
    readonly preparation: string;
    readonly prepared: string;
    private readonly backup: string;
    private readonly journal: string;
    private readonly warn: (message: string) => void;
    private readonly removeObsolete: (path: string) => Promise<void>;
    private releaseLock: (() => Promise<void>) | undefined;

    constructor(destination: string,
        warn: (message: string) => void = () => {},
        removeObsolete: (path: string) => Promise<void> =
            path => rm(path, { recursive: true, force: true })) {
        this.destination = destination;
        this.warn = warn;
        this.removeObsolete = removeObsolete;
        const key = createHash('sha256').update(destination).digest('hex').slice(0, 16);
        this.work = join(dirname(destination), `.simplex-install-${key}`);
        this.preparation = join(this.work, `attempt-${randomUUID()}`);
        this.prepared = join(this.preparation, 'prepared');
        this.backup = join(this.preparation, 'previous');
        this.journal = join(this.work, 'transaction.json');
    }

    /** Recover under the destination lock; obsolete cleanup failures are warnings, not recovery failures. */
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
            await this.cleanupObsolete();
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
        const record = JSON.parse(await readFile(this.journal, 'utf8')) as {
            hadDestination: boolean; attempt?: string;
        };
        if (!record || typeof record.hadDestination !== 'boolean'
            || (record.attempt !== undefined && (typeof record.attempt !== 'string'
                || !/^attempt-[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/.test(record.attempt)))) {
            throw new Error('Invalid installation recovery journal');
        }
        // Older PR revisions wrote prepared/previous directly under work.
        const recovery = record.attempt ? join(this.work, record.attempt) : this.work;
        if (!(await lstat(recovery)).isDirectory()) throw new Error('Invalid installation recovery directory');
        const backup = join(recovery, 'previous');
        const prepared = join(recovery, 'prepared');
        if (await exists(backup)) {
            if (!(await lstat(backup)).isDirectory()) throw new Error('Invalid installation backup');
            await rm(this.destination, { recursive: true, force: true });
            await rename(backup, this.destination);
            await syncDirectory(dirname(this.destination));
            await syncDirectory(recovery);
        } else if (!record.hadDestination && !(await exists(prepared))) {
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
        await mkdir(this.preparation, { mode: 0o700, recursive: true });
        await rename(tree, this.prepared);
        const hadDestination = await exists(this.destination);
        await atomicWrite(this.journal, `${JSON.stringify({ hadDestination,
            attempt: basename(this.preparation) })}\n`);
        try {
            if (hadDestination) await rename(this.destination, this.backup);
            await syncDirectory(dirname(this.destination));
            await syncDirectory(this.preparation);
            await beforePublish?.();
            await rename(this.prepared, this.destination);
            await syncDirectory(dirname(this.destination));
            await syncDirectory(this.preparation);
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

    /**
     * No journal means these entries are obsolete, including a committed old
     * backup. A deletion failure retains its path and only emits a warning.
     * The next invocation retries cleanup and uses a different attempt directory.
     */
    private async cleanupObsolete(): Promise<void> {
        let entries: string[];
        try {
            entries = await readdir(this.work);
        } catch (error) {
            this.warn(`Could not inspect obsolete installer files in ${this.work}: ${(error as Error).message}`);
            return;
        }
        for (const entry of entries) {
            if (entry === 'lock' || entry === 'transaction.json') continue;
            const path = join(this.work, entry);
            try {
                await this.removeObsolete(path);
            } catch (error) {
                this.warn(`Could not remove obsolete installer files; retained at ${path}: ${(error as Error).message}`);
            }
        }
    }

    /** Preserve genuine recovery files on rollback failure; obsolete cleanup never fails the installation. */
    async close(): Promise<void> {
        if (!this.releaseLock) return;
        try {
            if (!(await exists(this.journal))) {
                await this.cleanupObsolete();
            }
        } finally {
            const release = this.releaseLock;
            this.releaseLock = undefined;
            await release();
        }
    }
}
