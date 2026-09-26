/**
 * @file hub configuration: defaults, loading, path resolution, validation.
 *
 * The hub uses one JSON document with optional `//` and block comments. A
 * comment-tolerant reader keeps the file self-documenting without adding a
 * YAML dependency; the same document stays valid JSON after comments are
 * stripped. Relative paths are resolved against the directory of the
 * configuration file, mirroring the worker's own rule (`load/README.md`);
 * command-line overrides resolve against the process working directory.
 *
 * The worker configuration the hub *generates* is a different document: see
 * src/launch/config-render.ts.
 */
import { existsSync, readFileSync } from 'node:fs';
import { SCENARIOS } from './mock/provider.ts';
import type { Scenario } from './mock/provider.ts';
import { dirname, isAbsolute, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

/** Absolute path of the `hub/` package directory. */
export const hubRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');

/** Configuration error raised for unusable files or values. */
export class ConfigError extends Error {
    constructor(message: string) {
        super(message);
        this.name = 'ConfigError';
    }
}

/** Launcher kinds the hub understands; see src/launch/launcher.ts. */
export const LAUNCHER_KINDS = ['simplex-worker', 'command'] as const;

/** One launcher kind. */
export type LauncherKind = (typeof LAUNCHER_KINDS)[number];

/**
 * One provider profile, copied into the generated worker configuration.
 *
 * The hub reads only `plugin` and `model`; the rest is the provider plugin's
 * own vocabulary, which is why the index signature is there.
 */
export interface ProviderProfile {
    plugin?: string;
    model: string;
    [field: string]: unknown;
}

/** The hub's complete configuration, exactly as `defaultConfig()` produces it. */
export interface HubConfig {
    listen: { host: string; port: number };
    dataDir: string;
    /** Empty token disables panel authentication on a loopback listener. */
    panel: { token: string };
    worker: {
        bin: string;
        args: string[];
        threads: number;
        /**
         * Host the hub advertises to workers, when it is not the one it bound.
         *
         * A hub that listens on `0.0.0.0` cannot advertise that address — a
         * worker would try to connect to itself — so the default has always been
         * "loopback for a wildcard bind", which is wrong the moment a worker is
         * not on this machine. This is the address to use instead: a bridge
         * address (`172.17.0.1`) for a container, a LAN name for a fleet.
         */
        connectHost: string;
        promptsDir: string;
        systemPromptFile: string;
        maxExchanges: number;
        eventCapacity: number;
        confirmationTimeoutMs: number;
        payloadCapacity: number;
        signalCapacity: number;
        writeCapacity: number;
        initialBackoffMs: number;
        maxBackoffMs: number;
        idleTimeoutSeconds: number;
        stopTimeoutMs: number;
        sigtermGraceMs: number;
        sigkillGraceMs: number;
        persistence: { enabled: boolean; readable: boolean };
        environment: { workspace: string; platform: string; software: string[] };
    };
    providerProfiles: Record<string, ProviderProfile>;
    launcher: {
        kind: LauncherKind;
        command: string[];
        args: string[];
        config: 'hub' | 'launcher';
        cwd: string;
        pidFile: string;
    };
    mock: {
        enabled: boolean;
        listen: string;
        profile: string;
        scenario: Scenario;
        slowMs: number;
        /** The command `auto` proposes; empty uses the provider's default. */
        toolCommand: string;
    };
    limits: {
        transcriptEvents: number;
        transcriptBytes: number;
        logLines: number;
        logRingBytes: number;
        logBytes: number;
        logFiles: number;
        maxMessageBytes: number;
        pingIntervalMs: number;
        confirmIdentityHoldMs: number;
    };
    forceKillProcessGroup: boolean;
}

/** A fresh default configuration object; never shared or mutated in place. */
export function defaultConfig(): HubConfig {
    return {
        listen: { host: '127.0.0.1', port: 8800 },
        dataDir: './data',
        // Empty string disables panel authentication. Non-loopback listeners
        // require a token (validated below): the worker-facing payload channel
        // is an approval authority (core/docs/worker-protocol.md).
        panel: { token: '' },
        worker: {
            bin: '../build/bin/simplex_worker',
            args: [],
            threads: 1,
            // Empty means "derive it from the listener": loopback, or the host
            // it is bound to.
            connectHost: '',
            promptsDir: '../build/bin/prompts',
            systemPromptFile: 'coding_agent.yaml',
            maxExchanges: 512,
            eventCapacity: 1024,
            confirmationTimeoutMs: 120000,
            // Transport settings copied into every generated worker config;
            // the defaults are the worker's own (core/docs/worker-protocol.md).
            payloadCapacity: 256,
            signalCapacity: 256,
            writeCapacity: 256,
            initialBackoffMs: 250,
            maxBackoffMs: 10000,
            idleTimeoutSeconds: 0,
            // How long a graceful stop may take before the hub escalates.
            stopTimeoutMs: 15000,
            // Grace periods for the escalating stop: SIGTERM first, then
            // SIGKILL (optionally to the process group).
            sigtermGraceMs: 5000,
            sigkillGraceMs: 2000,
            persistence: { enabled: true, readable: false },
            environment: { workspace: '', platform: '', software: [] },
        },
        // Maps directly onto the worker's `providers` + `driver_model`.
        providerProfiles: {
            deepseek: {
                plugin: 'deepseek',
                model: 'deepseek-flash',
                config: { reasoning: { effort: 'high' } },
                endpoint: {
                    base_url: 'https://api.deepseek.com',
                    auth: { scheme: 'bearer', api_key: '${DEEPSEEK_API_KEY}' },
                },
                retry: { max_attempts: 3, initial_backoff_ms: 500, max_backoff_ms: 120000 },
            },
            mock: {
                plugin: 'deepseek',
                model: 'mock-flash',
                endpoint: {
                    base_url: 'http://127.0.0.1:0',
                    auth: { scheme: 'none' },
                },
                retry: { max_attempts: 0 },
            },
        },
        launcher: {
            kind: 'simplex-worker',
            // `command` launcher only: template array, e.g.
            // ["bash", "scripts/simplex-run.sh", "{session}", "{config}"].
            command: [],
            // `simplex-worker` only: extra arguments appended after the
            // generated --config/--session/--threads.
            args: [],
            // "hub": the hub renders config.yaml; "launcher": the launcher owns
            // configuration and only receives session/data-dir endpoints.
            config: 'hub',
            // Working directory for the spawned command; empty means the
            // per-session directory.
            cwd: '',
            // Set when the launcher daemonizes: the supervisor then signals the
            // pid in this file instead of the process it spawned.
            pidFile: '',
        },
        mock: {
            enabled: false,
            listen: '127.0.0.1:0',
            // Provider profile rewritten with the resolved mock address.
            profile: 'mock',
            // Fallback scenario for a model name without a mock-* suffix.
            scenario: 'auto',
            // Delay used by the "slow" scenario, in milliseconds.
            slowMs: 1500,
            // The command the `auto` scenario proposes. Its default writes to
            // the working directory; overriding it with something like
            // `hostname` is how a containerised worker demo shows *where* the
            // tool ran rather than asserting that it did.
            toolCommand: '',
        },
        limits: {
            // In-memory budgets per session: an entry count plus a byte ceiling,
            // because a single model response can dwarf a thousand small events.
            transcriptEvents: 5000,
            transcriptBytes: 32 * 1024 * 1024,
            logLines: 500,
            logRingBytes: 256 * 1024,
            // On-disk worker log rotation.
            logBytes: 8 * 1024 * 1024,
            logFiles: 2,
            maxMessageBytes: 32 * 1024 * 1024,
            // Server-side WebSocket ping interval for worker connections, in
            // milliseconds; zero disables it. A half-open socket must not keep
            // looking like a live worker.
            pingIntervalMs: 30000,
            // How long a confirmation with an unverified worker identity is
            // held before it is denied. Always clamped to the confirmation
            // deadline; see src/worker/confirmation.ts.
            confirmIdentityHoldMs: 15000,
        },
        // Off by default: SIGKILL to the worker's process group also kills its
        // descendants, which the worker's own cleanup deliberately does not
        // promise. Enable it, or use the panel's explicit force-kill action.
        forceKillProcessGroup: false,
    };
}

/** True for a plain JSON object (not an array, not null). */
function isPlainObject(value: unknown): value is Record<string, unknown> {
    return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/**
 * Subtrees whose shape belongs to the user rather than to the hub.
 *
 * A provider profile is handed to the worker largely as written, so its keys
 * are the plugin's vocabulary, not the hub's.
 */
const OPAQUE_CONFIG_PATHS = new Set(['providerProfiles']);

/**
 * Collect configuration keys that the hub does not know.
 *
 * The defaults *are* the schema. Deriving it by walking them means a new option
 * cannot be added without automatically being accepted, and a misspelled or
 * stale key is reported instead of being merged in and silently ignored — the
 * failure mode that makes a typo in `limits.logLines` look like a hub bug.
 */
function unknownConfigKeys(
    value: object,
    shape: object,
    prefix = '',
): string[] {
    const found: string[] = [];
    const expectedKeys = shape as Record<string, unknown>;
    for (const [key, item] of Object.entries(value)) {
        const path = prefix ? `${prefix}.${key}` : key;
        const expected = expectedKeys[key];
        if (expected === undefined) {
            found.push(path);
            continue;
        }
        if (OPAQUE_CONFIG_PATHS.has(path)) continue;
        if (isPlainObject(item) && isPlainObject(expected)) {
            found.push(...unknownConfigKeys(item, expected, path));
        }
    }
    return found;
}

/**
 * Recursively overlay `override` onto `base`; arrays replace, objects merge.
 *
 * The return type is the base's: an override may add, change, or omit keys, but
 * what comes out is a whole configuration, and saying so is what lets the rest
 * of the hub read it without a null check on every field.
 */
export function mergeConfig<T>(base: T, override: unknown): T {
    if (!isPlainObject(override)) return override as T;
    const result: Record<string, unknown> = isPlainObject(base) ? { ...base } : {};
    for (const [key, value] of Object.entries(override)) {
        const existing = isPlainObject(base) ? base[key] : undefined;
        result[key] = isPlainObject(value) && isPlainObject(existing)
            ? mergeConfig(existing, value)
            : value;
    }
    return result as T;
}

/**
 * Remove `//` and block comments from JSON text without touching string
 * contents. Removed characters become spaces so byte offsets — and therefore
 * parse-error positions — stay meaningful.
 *
 * @returns JSON text with comments blanked out.
 */
export function stripJsonComments(text: string): string {
    let out = '';
    let index = 0;
    let inString = false;
    let escaped = false;
    while (index < text.length) {
        const char = text[index] as string;
        if (inString) {
            out += char;
            if (escaped) escaped = false;
            else if (char === '\\') escaped = true;
            else if (char === '"') inString = false;
            index += 1;
            continue;
        }
        if (char === '"') {
            inString = true;
            out += char;
            index += 1;
            continue;
        }
        if (char === '/' && text[index + 1] === '/') {
            while (index < text.length && text[index] !== '\n') {
                out += ' ';
                index += 1;
            }
            continue;
        }
        if (char === '/' && text[index + 1] === '*') {
            while (index < text.length && !(text[index] === '*' && text[index + 1] === '/')) {
                out += text[index] === '\n' ? '\n' : ' ';
                index += 1;
            }
            out += '  ';
            index += 2;
            continue;
        }
        out += char;
        index += 1;
    }
    return out;
}

/** Parse comment-tolerant JSON, reporting the file and offset on failure. */
export function parseConfigText(text: string, file: string): Record<string, unknown> {
    try {
        const value: unknown = JSON.parse(stripJsonComments(text));
        if (!isPlainObject(value)) {
            throw new ConfigError(`${file}: configuration must be a JSON object`);
        }
        return value;
    } catch (error) {
        if (error instanceof ConfigError) throw error;
        const message = error instanceof Error ? error.message : String(error);
        throw new ConfigError(`${file}: ${message}`);
    }
}

/** Resolve a possibly relative path against `base`; absolute paths pass through. */
export function resolveAgainst(base: string, value: string): string {
    return isAbsolute(value) ? value : resolve(base, value);
}

/** Apply path resolution to a merged configuration. */
function resolvePaths(config: HubConfig, baseDir: string): HubConfig {
    config.dataDir = resolveAgainst(baseDir, config.dataDir);
    config.worker.bin = resolveAgainst(baseDir, config.worker.bin);
    config.worker.promptsDir = resolveAgainst(baseDir, config.worker.promptsDir);
    if (config.worker.environment.workspace) {
        config.worker.environment.workspace =
            resolveAgainst(baseDir, config.worker.environment.workspace);
    }
    return config;
}

/** Throw a ConfigError unless `condition` holds. */
function check(condition: unknown, message: string): asserts condition {
    if (!condition) throw new ConfigError(message);
}

/** Validate a merged configuration; throws ConfigError with a specific reason. */
export function validateConfig(config: HubConfig): HubConfig {
    check(isPlainObject(config), 'configuration must be an object');
    const unknown = unknownConfigKeys(config, defaultConfig());
    check(unknown.length === 0,
        `unknown configuration ${unknown.length === 1 ? 'key' : 'keys'}: ${unknown.join(', ')}`);
    check(typeof config.listen?.host === 'string' && config.listen.host.length > 0,
        'listen.host must be a nonempty string');
    const port = config.listen?.port;
    check(Number.isInteger(port) && port >= 0 && port <= 65535,
        'listen.port must be an integer between 0 and 65535');
    check(typeof config.dataDir === 'string' && config.dataDir.length > 0,
        'dataDir must be a nonempty string');
    check(typeof config.panel?.token === 'string', 'panel.token must be a string');

    const localOnly = ['127.0.0.1', '::1', 'localhost'].includes(config.listen.host);
    if (!localOnly && config.panel.token.length === 0) {
        throw new ConfigError(
            `listen.host ${config.listen.host} is not loopback: set panel.token, because a `
            + 'payload channel grants tool-approval authority (core/docs/worker-protocol.md)');
    }

    const worker = config.worker;
    check(typeof worker.bin === 'string' && worker.bin.length > 0, 'worker.bin must be a path');
    check(Array.isArray(worker.args), 'worker.args must be an array of strings');
    check(worker.args.every((value) => typeof value === 'string'),
        'worker.args must contain only strings');
    check(Number.isInteger(worker.threads) && worker.threads >= 1,
        'worker.threads must be a positive integer');
    check(typeof worker.connectHost === 'string', 'worker.connectHost must be a string');
    // A bare host, because it is pasted into a URL next to a port. Accepting
    // `http://host` here would produce `ws://http://host:8800/...` two layers
    // down, which is a confusing way to learn about a typo.
    check(worker.connectHost === '' || !/[/:?#\s]/.test(worker.connectHost),
        'worker.connectHost must be a bare host or address, without a scheme, port or path');
    check(Number.isInteger(worker.maxExchanges) && worker.maxExchanges > 0,
        'worker.maxExchanges must be a positive integer');
    check(Number.isInteger(worker.eventCapacity) && worker.eventCapacity > 0,
        'worker.eventCapacity must be a positive integer');
    check(Number.isInteger(worker.confirmationTimeoutMs) && worker.confirmationTimeoutMs > 0,
        'worker.confirmationTimeoutMs must be a positive integer');
    for (const key of ['payloadCapacity', 'signalCapacity', 'writeCapacity',
        'initialBackoffMs', 'maxBackoffMs', 'idleTimeoutSeconds', 'stopTimeoutMs',
        'sigtermGraceMs', 'sigkillGraceMs'] as const) {
        const value = worker[key];
        check(Number.isInteger(value) && value >= 0,
            `worker.${key} must be a nonnegative integer`);
    }
    check(worker.maxBackoffMs >= worker.initialBackoffMs,
        'worker.maxBackoffMs must be at least worker.initialBackoffMs');
    check(worker.stopTimeoutMs > 0, 'worker.stopTimeoutMs must be positive');
    check(worker.sigtermGraceMs > 0 && worker.sigkillGraceMs > 0,
        'worker.sigtermGraceMs and worker.sigkillGraceMs must be positive');
    check(typeof worker.persistence?.enabled === 'boolean',
        'worker.persistence.enabled must be a boolean');

    check(isPlainObject(config.providerProfiles), 'providerProfiles must be an object');
    check(Object.keys(config.providerProfiles).length > 0,
        'providerProfiles must define at least one profile');
    for (const [name, profile] of Object.entries(config.providerProfiles)) {
        check(isPlainObject(profile), `providerProfiles.${name} must be an object`);
        check(typeof profile.plugin === 'string' || name.length > 0,
            `providerProfiles.${name}.plugin must be a string when present`);
        check(typeof profile.model === 'string' && profile.model.length > 0,
            `providerProfiles.${name}.model must be a nonempty string`);
    }

    check((LAUNCHER_KINDS as readonly string[]).includes(config.launcher?.kind),
        `launcher.kind must be one of ${LAUNCHER_KINDS.join(', ')}`);
    check((Array.isArray(config.launcher.command) && config.launcher.command.length > 0)
        || config.launcher.kind !== 'command',
    'launcher.command must be a nonempty template array for the command launcher');
    check(['hub', 'launcher'].includes(config.launcher.config),
        'launcher.config must be "hub" or "launcher"');
    check(typeof config.launcher.cwd === 'string', 'launcher.cwd must be a string');
    check(typeof config.launcher.pidFile === 'string', 'launcher.pidFile must be a string');
    check(config.launcher.pidFile === '' || config.launcher.pidFile.startsWith('/'),
        'launcher.pidFile must be an absolute path');

    check(typeof config.mock?.enabled === 'boolean', 'mock.enabled must be a boolean');
    check((SCENARIOS as readonly string[]).includes(config.mock?.scenario),
        `mock.scenario must be one of ${SCENARIOS.join(', ')}`);
    check(Number.isInteger(config.mock?.slowMs) && config.mock.slowMs >= 0,
        'mock.slowMs must be a nonnegative integer');
    check(typeof config.mock?.toolCommand === 'string',
        'mock.toolCommand must be a string');
    check(config.mock.toolCommand === '' || config.mock.toolCommand.trim().length > 0,
        'mock.toolCommand must not be blank; leave it empty to use the default');
    if (config.mock.enabled) {
        check(typeof config.providerProfiles[config.mock.profile] === 'object',
            `mock.profile "${config.mock.profile}" is not a configured provider profile`);
    }

    const limits = config.limits;
    for (const key of ['transcriptEvents', 'transcriptBytes', 'logLines', 'logRingBytes',
        'logBytes', 'logFiles', 'maxMessageBytes'] as const) {
        const value = limits[key];
        check(Number.isInteger(value) && value > 0,
            `limits.${key} must be a positive integer`);
    }
    check(Number.isInteger(limits.confirmIdentityHoldMs) && limits.confirmIdentityHoldMs >= 0,
        'limits.confirmIdentityHoldMs must be a nonnegative integer');
    check(Number.isInteger(limits.pingIntervalMs) && limits.pingIntervalMs >= 0,
        'limits.pingIntervalMs must be a nonnegative integer');
    check(limits.confirmIdentityHoldMs < config.worker.confirmationTimeoutMs,
        'limits.confirmIdentityHoldMs must be shorter than worker.confirmationTimeoutMs');
    check(typeof config.forceKillProcessGroup === 'boolean',
        'forceKillProcessGroup must be a boolean');
    return config;
}

/** Recursively optional, for the parts of a configuration a caller may set. */
export type DeepPartial<T> = {
    [K in keyof T]?: T[K] extends readonly unknown[]
        ? T[K]
        : T[K] extends object
            ? DeepPartial<T[K]>
            : T[K];
};

/** What `loadConfig` accepts. */
export interface LoadConfigOptions {
    /** Configuration path; when omitted, an existing config beside the package is used. */
    file?: string | undefined;
    /** Command-line overrides, already shaped like the configuration. */
    overrides?: DeepPartial<HubConfig>;
    /** Directory for override path resolution; defaults to the process cwd. */
    cwd?: string;
    /** Fail when no configuration file exists. */
    requireFile?: boolean;
}

/** What `loadConfig` returns. */
export interface LoadedConfig {
    config: HubConfig;
    file: string | null;
    baseDir: string;
    overrideDir: string;
}

/**
 * Load, merge, resolve, and validate the hub configuration.
 */
export function loadConfig({
    file,
    overrides = {},
    cwd = process.cwd(),
    requireFile = false,
}: LoadConfigOptions = {}): LoadedConfig {
    let selected = file ? resolve(cwd, file) : null;
    if (!selected) {
        for (const candidate of ['hub.config.jsonc', 'hub.config.json']) {
            const path = resolve(hubRoot, candidate);
            if (existsSync(path)) {
                selected = path;
                break;
            }
        }
    } else if (!existsSync(selected)) {
        throw new ConfigError(`configuration file not found: ${selected}`);
    }
    if (!selected && requireFile) {
        throw new ConfigError(`configuration file not found (looked in ${hubRoot})`);
    }

    const baseDir = selected ? dirname(selected) : hubRoot;
    const fromFile = selected
        ? parseConfigText(readFileSync(selected, 'utf8'), selected)
        : {};

    const merged = mergeConfig(mergeConfig(defaultConfig(), fromFile), overrides);
    resolvePaths(merged, baseDir);
    // Override paths arrive from the command line: resolve them against cwd,
    // which is what a user typing `--data-dir ./x` expects.
    if (overrides.dataDir) merged.dataDir = resolveAgainst(cwd, overrides.dataDir);
    if (overrides.worker?.bin) merged.worker.bin = resolveAgainst(cwd, overrides.worker.bin);
    if (overrides.worker?.promptsDir) {
        merged.worker.promptsDir = resolveAgainst(cwd, overrides.worker.promptsDir);
    }
    validateConfig(merged);
    return { config: merged, file: selected, baseDir, overrideDir: cwd };
}
