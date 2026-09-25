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
 * src/launch/config-render.js.
 */
import { existsSync, readFileSync } from 'node:fs';
import { SCENARIOS } from './mock/provider.js';
import { dirname, isAbsolute, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

/** Absolute path of the `hub/` package directory. */
export const hubRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');

/** Configuration error raised for unusable files or values. */
export class ConfigError extends Error {
    constructor(message) {
        super(message);
        this.name = 'ConfigError';
    }
}

/** Launcher kinds the hub understands; see src/launch/launcher.js. */
export const LAUNCHER_KINDS = ['simplex-worker', 'command'];

/** A fresh default configuration object; never shared or mutated in place. */
export function defaultConfig() {
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
        },
        limits: {
            transcriptEvents: 5000,
            logLines: 500,
            logBytes: 8 * 1024 * 1024,
            logFiles: 2,
            maxMessageBytes: 32 * 1024 * 1024,
            // Server-side WebSocket ping interval for worker connections, in
            // milliseconds; zero disables it. A half-open socket must not keep
            // looking like a live worker.
            pingIntervalMs: 30000,
            // How long a confirmation with an unverified worker identity is
            // held before it is denied. Always clamped to the confirmation
            // deadline; see src/worker/confirmation.js.
            confirmIdentityHoldMs: 15000,
        },
        // Off by default: SIGKILL to the worker's process group also kills its
        // descendants, which the worker's own cleanup deliberately does not
        // promise. Enable it, or use the panel's explicit force-kill action.
        forceKillProcessGroup: false,
    };
}

/** True for a plain JSON object (not an array, not null). */
function isPlainObject(value) {
    return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/** Recursively overlay `override` onto `base`; arrays replace, objects merge. */
export function mergeConfig(base, override) {
    if (!isPlainObject(override)) return override;
    const result = isPlainObject(base) ? { ...base } : {};
    for (const [key, value] of Object.entries(override)) {
        result[key] = isPlainObject(value) && isPlainObject(base?.[key])
            ? mergeConfig(base[key], value)
            : value;
    }
    return result;
}

/**
 * Remove `//` and block comments from JSON text without touching string
 * contents. Removed characters become spaces so byte offsets — and therefore
 * parse-error positions — stay meaningful.
 *
 * @param {string} text
 * @returns {string} JSON text with comments blanked out.
 */
export function stripJsonComments(text) {
    let out = '';
    let index = 0;
    let inString = false;
    let escaped = false;
    while (index < text.length) {
        const char = text[index];
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
export function parseConfigText(text, file) {
    try {
        const value = JSON.parse(stripJsonComments(text));
        if (!isPlainObject(value)) {
            throw new ConfigError(`${file}: configuration must be a JSON object`);
        }
        return value;
    } catch (error) {
        if (error instanceof ConfigError) throw error;
        throw new ConfigError(`${file}: ${error.message}`);
    }
}

/** Resolve a possibly relative path against `base`; absolute paths pass through. */
export function resolveAgainst(base, value) {
    return isAbsolute(value) ? value : resolve(base, value);
}

/** Apply path resolution to a merged configuration. */
function resolvePaths(config, baseDir) {
    const paths = {
        dataDir: resolveAgainst(baseDir, config.dataDir),
        workerBin: resolveAgainst(baseDir, config.worker.bin),
        promptsDir: resolveAgainst(baseDir, config.worker.promptsDir),
    };
    config.dataDir = paths.dataDir;
    config.worker.bin = paths.workerBin;
    config.worker.promptsDir = paths.promptsDir;
    if (config.worker.environment.workspace) {
        config.worker.environment.workspace =
            resolveAgainst(baseDir, config.worker.environment.workspace);
    }
    return config;
}

/** Throw a ConfigError unless `condition` holds. */
function check(condition, message) {
    if (!condition) throw new ConfigError(message);
}

/** Validate a merged configuration; throws ConfigError with a specific reason. */
export function validateConfig(config) {
    check(isPlainObject(config), 'configuration must be an object');
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

    const worker = config.worker ?? {};
    check(typeof worker.bin === 'string' && worker.bin.length > 0, 'worker.bin must be a path');
    check(Array.isArray(worker.args), 'worker.args must be an array of strings');
    check(worker.args.every((value) => typeof value === 'string'),
        'worker.args must contain only strings');
    check(Number.isInteger(worker.threads) && worker.threads >= 1,
        'worker.threads must be a positive integer');
    check(Number.isInteger(worker.maxExchanges) && worker.maxExchanges > 0,
        'worker.maxExchanges must be a positive integer');
    check(Number.isInteger(worker.eventCapacity) && worker.eventCapacity > 0,
        'worker.eventCapacity must be a positive integer');
    check(Number.isInteger(worker.confirmationTimeoutMs) && worker.confirmationTimeoutMs > 0,
        'worker.confirmationTimeoutMs must be a positive integer');
    for (const key of ['payloadCapacity', 'signalCapacity', 'writeCapacity',
        'initialBackoffMs', 'maxBackoffMs', 'idleTimeoutSeconds', 'stopTimeoutMs',
        'sigtermGraceMs', 'sigkillGraceMs']) {
        check(Number.isInteger(worker[key]) && worker[key] >= 0,
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

    check(LAUNCHER_KINDS.includes(config.launcher?.kind),
        `launcher.kind must be one of ${LAUNCHER_KINDS.join(', ')}`);
    check(Array.isArray(config.launcher.command) && config.launcher.command.length > 0
        || config.launcher.kind !== 'command',
    'launcher.command must be a nonempty template array for the command launcher');
    check(['hub', 'launcher'].includes(config.launcher.config),
        'launcher.config must be "hub" or "launcher"');
    check(typeof config.launcher.cwd === 'string', 'launcher.cwd must be a string');
    check(typeof config.launcher.pidFile === 'string', 'launcher.pidFile must be a string');
    check(config.launcher.pidFile === '' || config.launcher.pidFile.startsWith('/'),
        'launcher.pidFile must be an absolute path');

    check(typeof config.mock?.enabled === 'boolean', 'mock.enabled must be a boolean');
    check(SCENARIOS.includes(config.mock?.scenario), `mock.scenario must be one of ${SCENARIOS.join(', ')}`);
    check(Number.isInteger(config.mock?.slowMs) && config.mock.slowMs >= 0,
        'mock.slowMs must be a nonnegative integer');
    if (config.mock.enabled) {
        check(typeof config.providerProfiles[config.mock.profile] === 'object',
            `mock.profile "${config.mock.profile}" is not a configured provider profile`);
    }

    const limits = config.limits ?? {};
    for (const key of ['transcriptEvents', 'logLines', 'logBytes', 'logFiles', 'maxMessageBytes']) {
        check(Number.isInteger(limits[key]) && limits[key] > 0,
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

/**
 * Load, merge, resolve, and validate the hub configuration.
 *
 * @param {object} [options]
 * @param {string} [options.file] configuration path; when omitted, an existing
 *   `hub.config.jsonc`/`hub.config.json` beside the package is used if present.
 * @param {object} [options.overrides] command-line overrides, already shaped
 *   like the configuration (relative paths resolve against `cwd`).
 * @param {string} [options.cwd] directory for override path resolution.
 * @param {boolean} [options.requireFile] fail when no configuration file exists.
 * @returns {{config: object, file: string|null, baseDir: string, overrideDir: string}}
 */
export function loadConfig({ file, overrides = {}, cwd = process.cwd(), requireFile = false } = {}) {
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
