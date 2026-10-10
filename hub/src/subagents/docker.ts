/** Foreground Docker lifetimes retain only the environment needed for management. */
import { execFile } from 'node:child_process';
import { accessSync, constants } from 'node:fs';
import { basename, delimiter, isAbsolute, resolve } from 'node:path';

/** Private startup snapshot, never included in public process descriptions. */
export interface DockerManagement {
    executable: string;
    cwd: string;
    env: Record<string, string>;
    name: string;
    /** Explicit additional names, retained so restart does not use current defaults. */
    passThrough: string[];
}

/** Docker connection/configuration, TLS, SSH, proxy and host lookup requirements.
 * Keep this list aligned with docs/hub/configurations.md. Never use DOCKER_* or
 * another wildcard: unrelated variables may contain model/application secrets. */
const MANAGEMENT_ENV = [
    'DOCKER_HOST', 'DOCKER_CONTEXT', 'DOCKER_CONFIG', 'DOCKER_CERT_PATH',
    'DOCKER_TLS', 'DOCKER_TLS_VERIFY', 'DOCKER_API_VERSION', 'DOCKER_CUSTOM_HEADERS',
    'PATH', 'HOME', 'USER', 'LOGNAME', 'SSH_AUTH_SOCK',
    'HTTP_PROXY', 'HTTPS_PROXY', 'NO_PROXY', 'http_proxy', 'https_proxy', 'no_proxy',
    'SSL_CERT_FILE', 'SSL_CERT_DIR',
    'SYSTEMROOT', 'SystemRoot', 'WINDIR', 'USERPROFILE', 'HOMEDRIVE', 'HOMEPATH',
    'APPDATA', 'LOCALAPPDATA', 'PATHEXT',
] as const;

/** Additional management variables require individual names, never patterns. */
export function validDockerManagementEnv(value: unknown): value is string[] {
    return Array.isArray(value)
        && value.every(key => typeof key === 'string' && /^[A-Za-z_][A-Za-z0-9_]*$/.test(key))
        && new Set(value).size === value.length;
}

function managementEnvironment(env: Record<string, string>, passThrough: string[]): Record<string, string> {
    return Object.fromEntries([...new Set<string>([...MANAGEMENT_ENV, ...passThrough])]
        .filter(key => Object.hasOwn(env, key)).map(key => [key, env[key]!]));
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
}, passThrough: string[] = []): DockerManagement | null {
    const name = dockerName(invocation);
    if (!name) return null;
    if (!validDockerManagementEnv(passThrough)) throw new Error('invalid Docker management environment names');
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
    return { executable, cwd, env: managementEnvironment(env, passThrough), name, passThrough: [...passThrough] };
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
    // Legacy snapshots captured the whole environment. Filter those too, without
    // filling absent values from this Hub's environment or changing the daemon.
    const passThrough = record.passThrough === undefined ? [] : record.passThrough;
    if (!validDockerManagementEnv(passThrough)) return null;
    return { executable: record.executable, cwd: record.cwd, name,
        env: managementEnvironment(env as Record<string, string>, passThrough), passThrough: [...passThrough] };
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
