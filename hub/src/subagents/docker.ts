/** Foreground Docker lifetimes are checked at the container, not its CLI process. */
import { execFile } from 'node:child_process';
import { basename } from 'node:path';

export function dockerName(record: { command: string; args: unknown[] }): string | null {
    if (basename(record.command) !== 'docker' || record.args[0] !== 'run') return null;
    const index = record.args.indexOf('--name');
    const name = record.args[index + 1];
    return index >= 0 && typeof name === 'string' && /^[A-Za-z0-9][A-Za-z0-9_.-]{0,200}$/.test(name) ? name : null;
}

/** null means daemon/query failure: never delete data on that evidence. */
export function dockerRunning(name: string): Promise<boolean | null> {
    return new Promise(resolve => execFile('docker', ['inspect', '--format', '{{.State.Running}}', name],
        { timeout: 2000, maxBuffer: 4096 }, (error, stdout, stderr) => {
            if (!error && stdout.trim() === 'true') resolve(true);
            else if (!error && stdout.trim() === 'false') resolve(false);
            else if (error && /No such (?:object|container)/i.test(stderr)) resolve(false);
            else resolve(null);
        }));
}

export function signalContainer(name: string, signal: 'TERM' | 'KILL'): Promise<void> {
    return new Promise(resolve => execFile('docker', ['kill', '--signal', signal, name],
        { timeout: 2000, maxBuffer: 4096 }, () => resolve()));
}
