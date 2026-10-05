/** Foreground Docker lifetimes use the executable/environment that launched them. */
import { execFile } from 'node:child_process';
import { accessSync, constants } from 'node:fs';
import { basename, delimiter, isAbsolute, resolve } from 'node:path';

/** Private startup snapshot, never included in public process descriptions. */
export interface DockerManagement {
    executable: string;
    cwd: string;
    env: Record<string, string>;
    name: string;
}

export function dockerName(record: { command: string; args: unknown[] }): string | null {
    if (basename(record.command) !== 'docker' || record.args[0] !== 'run') return null;
    const index = record.args.indexOf('--name');
    const name = record.args[index + 1];
    return index >= 0 && typeof name === 'string' && /^[A-Za-z0-9][A-Za-z0-9_.-]{0,200}$/.test(name) ? name : null;
}

/** Resolve once using startup PATH/cwd; later Hub defaults cannot select another CLI. */
export function captureDockerManagement(invocation: {
    command: string;
    args: string[];
    cwd: string;
    env: Record<string, string>;
}): DockerManagement | null {
    const name = dockerName(invocation);
    if (!name) return null;
    const env = Object.fromEntries(Object.entries({ ...process.env, ...invocation.env })
        .filter((entry): entry is [string, string] => typeof entry[1] === 'string'));
    const cwd = resolve(invocation.cwd || process.cwd());
    const candidates = invocation.command.includes('/') || isAbsolute(invocation.command)
        ? [resolve(cwd, invocation.command)]
        : (env.PATH ?? '/usr/bin:/bin').split(delimiter).map(path => resolve(cwd, path, invocation.command));
    const executable = candidates.find(path => {
        try { accessSync(path, constants.X_OK); return true; } catch { return false; }
    });
    if (!executable) throw new Error('cannot resolve Docker startup executable');
    return { executable, cwd, env, name };
}

/** Missing/invalid recovery context stays unknown; never fall back to another daemon. */
export function restoreDockerManagement(value: unknown, name: string): DockerManagement | null {
    if (!value || typeof value !== 'object' || Array.isArray(value)) return null;
    const record = value as Record<string, unknown>;
    if (typeof record.executable !== 'string' || !isAbsolute(record.executable) || record.executable.includes('\0')
        || typeof record.cwd !== 'string' || !isAbsolute(record.cwd) || record.cwd.includes('\0')
        || record.name !== name || !record.env || typeof record.env !== 'object' || Array.isArray(record.env)) return null;
    const env = record.env as Record<string, unknown>;
    if (Object.entries(env).some(([key, value]) => !key || key.includes('=') || key.includes('\0')
        || typeof value !== 'string' || value.includes('\0'))) return null;
    return { executable: record.executable, cwd: record.cwd, name, env: { ...env } as Record<string, string> };
}

/** null means missing context/query failure: never delete data on that evidence. */
export function dockerRunning(context: DockerManagement | null): Promise<boolean | null> {
    if (!context) return Promise.resolve(null);
    return new Promise(resolve => execFile(context.executable,
        ['inspect', '--format', '{{.State.Running}}', context.name],
        { cwd: context.cwd, env: context.env, timeout: 2000, maxBuffer: 4096 }, (error, stdout, stderr) => {
            if (!error && stdout.trim() === 'true') resolve(true);
            else if (!error && stdout.trim() === 'false') resolve(false);
            else if (error && /No such (?:object|container)/i.test(stderr)) resolve(false);
            else resolve(null);
        }));
}

export function signalContainer(context: DockerManagement | null, signal: 'TERM' | 'KILL'): Promise<void> {
    if (!context) return Promise.resolve();
    return new Promise(resolve => execFile(context.executable, ['kill', '--signal', signal, context.name],
        { cwd: context.cwd, env: context.env, timeout: 2000, maxBuffer: 4096 }, () => resolve()));
}
