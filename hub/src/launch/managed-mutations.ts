/** Bound and serialize manager mutations without changing the target's fail-fast lock contract. */
import { createHash } from 'node:crypto';
import { dirname, posix, resolve } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import { ManagementError } from './management.ts';
import type { ManagedLauncherOptions } from './managed-launcher.ts';

/** Local namespaces share a fence even when profiles select different executables.
 * Remote identity includes transport options/environment because they select the
 * host or Docker daemon. Hash it so queue keys never retain printable credentials.
 */
export function mutationNamespace(launcher: ManagedLauncherOptions, directory: string): string {
    const target = launcher.target;
    const identity = target.kind === 'local'
        ? ['local', resolve(dirname(directory))]
        : [target.kind, target.command, target.args, target.kind === 'ssh' ? target.host : target.container,
            launcher.cwd, Object.entries(launcher.env).sort(([a], [b]) => a.localeCompare(b)),
            posix.dirname(directory)];
    return createHash('sha256').update(JSON.stringify(identity)).digest('hex');
}

/** One process-wide FIFO per target namespace; idle queues are released. */
const tails = new Map<string, Promise<void>>();

/** Queue time and execution share a deadline. Timed-out queued tasks never run;
 * an active task retains the fence until it settles, including transport abort.
 */
export async function serializeMutation<T>(key: string, timeoutMs: number,
    task: (signal: AbortSignal) => Promise<T>): Promise<T> {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    const previous = tails.get(key) ?? Promise.resolve();
    const work = previous.then(async () => {
        if (controller.signal.aborted) throw new ManagementError('transport', 'cancelled');
        return task(controller.signal);
    });
    const tail = work.then(() => {}, () => {});
    tails.set(key, tail);
    void tail.then(() => { if (tails.get(key) === tail) tails.delete(key); });
    try {
        // Returning early is safe only while waiting. Once dispatched, observe
        // the operation's actual result (its caller passes signal to transport).
        await new Promise<void>((resolve, reject) => {
            const aborted = () => reject(new ManagementError('transport', 'cancelled'));
            controller.signal.addEventListener('abort', aborted, { once: true });
            previous.then(() => {
                controller.signal.removeEventListener('abort', aborted);
                resolve();
            });
        });
        return await work;
    } finally { clearTimeout(timer); }
}

/** Retry only a manager-reported, unpublished busy refusal. Never retry an
 * ambiguous transport/protocol outcome. All retries together spend at most
 * 750 ms waiting and at most four calls; the enclosing deadline also applies.
 */
export async function retryManagerBusy<T>(task: () => Promise<T>, signal: AbortSignal): Promise<T> {
    for (let attempt = 0; ; ++attempt) {
        if (signal.aborted) throw new ManagementError('transport', 'cancelled');
        try { return await task(); }
        catch (error) {
            if (!(error instanceof ManagementError) || error.kind !== 'manager'
                || error.code !== 'busy' || error.published || attempt >= 3 || signal.aborted) throw error;
            await delay(25 * 2 ** attempt + Math.floor(Math.random() * 25), undefined, { signal });
        }
    }
}
