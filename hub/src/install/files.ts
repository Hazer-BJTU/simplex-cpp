/** Small Linux filesystem primitives shared by the two installer transactions. */
import { spawn } from 'node:child_process';
import { open, rename, rm, lstat } from 'node:fs/promises';
import { dirname } from 'node:path';
import { randomUUID } from 'node:crypto';

export async function exists(path: string): Promise<boolean> {
    try { await lstat(path); return true; }
    catch (error) {
        if ((error as NodeJS.ErrnoException).code === 'ENOENT') return false;
        throw error;
    }
}

/** Persist the directory entries used by rollback and crash recovery. */
export async function syncDirectory(path: string): Promise<void> {
    const handle = await open(path, 'r');
    try { await handle.sync(); }
    finally { await handle.close(); }
}

/** Same-directory rename publishes only a completely written file. */
export async function atomicWrite(path: string, contents: string, mode = 0o600): Promise<void> {
    const temporary = `${path}.${randomUUID()}.tmp`;
    const handle = await open(temporary, 'wx', mode);
    try {
        await handle.writeFile(contents);
        await handle.chmod(mode);
        await handle.sync();
        await handle.close();
        await rename(temporary, path);
        await syncDirectory(dirname(path));
    } finally {
        await handle.close();
        await rm(temporary, { force: true });
    }
}

/**
 * Hold a util-linux advisory lock in a child whose stdin is owned by this
 * process. EOF (including process death) releases it. Never unlink the lock
 * file: doing so would let a later caller lock a different inode concurrently.
 */
export async function acquireLock(path: string): Promise<() => Promise<void>> {
    if (await exists(path) && !(await lstat(path)).isFile()) throw new Error(`Invalid installer lock file: ${path}`);
    const handle = await open(path, 'a', 0o600);
    await handle.close();
    const child = spawn('flock', ['--exclusive', '--nonblock', path, process.execPath, '-e',
        "process.stdout.write('locked\\n'); process.stdin.resume();"], {
        // A terminal SIGINT must not release the lock before the parent finishes
        // publication/rollback. Parent death still closes stdin automatically.
        detached: true,
        stdio: ['pipe', 'pipe', 'pipe'],
    });
    // A broken pipe during failed acquisition must not become an unhandled error.
    child.stdin.on('error', () => {});
    let diagnostics = '';
    child.stderr.on('data', chunk => { if (diagnostics.length < 4096) diagnostics += String(chunk); });
    const closed = new Promise<void>(resolve => child.once('close', () => resolve()));
    try {
        await new Promise<void>((resolve, reject) => {
            const timer = setTimeout(() => { child.kill(); reject(new Error('Installer lock acquisition timed out')); }, 10_000);
            let settled = false;
            const finish = (error?: Error) => {
                if (settled) return;
                settled = true;
                clearTimeout(timer);
                error ? reject(error) : resolve();
            };
            child.once('error', error => finish(error));
            child.once('close', () => finish(new Error(`Another installer holds the lock, or flock failed: ${path} ${diagnostics.trim()}`)));
            let acknowledgement = '';
            child.stdout.on('data', chunk => {
                acknowledgement += String(chunk);
                if (acknowledgement.includes('\n')) {
                    finish(acknowledgement === 'locked\n' ? undefined : new Error('Invalid lock acknowledgement'));
                }
            });
        });
    } catch (error) {
        child.stdin.end();
        await closed;
        throw error;
    }
    return async () => {
        child.stdin.end();
        await closed;
    };
}
