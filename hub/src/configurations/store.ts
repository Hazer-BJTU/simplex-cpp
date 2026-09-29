/** Durable, editable configuration library. Files retain comments and are never
 * expanded in place: session-specific values belong in session snapshots. */
import { createHash, randomUUID } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, readdirSync, renameSync, rmSync, writeFileSync, lstatSync } from 'node:fs';
import { join } from 'node:path';
import { isMap, parseDocument } from 'yaml';
import { ConfigError, defaultConfig, hubRoot, mergeConfig, parseConfigText, validateConfig } from '../config.ts';
import { persistenceChild } from '../launch/config-file.ts';
import type { HubConfig } from '../config.ts';

import type { ConfigKind, ConfigFile } from '../../shared/configurations.ts';
export type { ConfigKind, ConfigFile } from '../../shared/configurations.ts';
import { renderSessionConfig } from '../launch/config-render.ts';
export interface LaunchDocument {
    launcher: HubConfig['launcher'];
    worker?: Partial<Pick<HubConfig['worker'], 'bin' | 'args' | 'threads' | 'connectHost' | 'stopTimeoutMs' | 'sigtermGraceMs' | 'sigkillGraceMs'>>;
    env?: Record<string, string>;
    /** Optional externally reachable WebSocket origins, including proxy prefix. */
    endpoints?: { events?: string; confirm?: string; tools?: string };
}

/** A status-bearing error is translated by the authenticated HTTP router. */
export function configError(message: string, status = 400): Error & { status: number } {
    return Object.assign(new Error(message), { status });
}

/** Publish one complete private file; callers perform revision checks first. */
export function writeConfigFile(path: string, text: string): void {
    const temporary = `${path}.${randomUUID()}.tmp`;
    try {
        writeFileSync(temporary, text, { flag: 'wx', mode: 0o600 });
        renameSync(temporary, path);
    } finally {
        rmSync(temporary, { force: true });
    }
}

/** Parse worker YAML with bounded aliases, retaining comments and unknown keys.
 * Only Hub-managed fields and selected model references are checked here;
 * plugin-specific values remain the worker's responsibility. */
export function workerDocument(text: string) {
    const doc = parseDocument(text);
    if (doc.errors.length) {
        const error = doc.errors[0];
        throw configError(`Invalid YAML at line ${error?.linePos?.[0]?.line ?? '?'}: ${error?.code}`);
    }
    if (!isMap(doc.contents)) throw configError('Worker configuration must be a YAML mapping');
    let value: Record<string, unknown>;
    try { value = doc.toJS({ maxAliasCount: 100 }); }
    catch { throw configError('Worker YAML exceeds the alias expansion limit'); }
    const providerExists = (name: unknown): boolean => {
        const providers = value.providers;
        if (typeof name !== 'string' || !providers || typeof providers !== 'object'
            || Array.isArray(providers) || !Object.hasOwn(providers, name)) return false;
        const provider = (providers as Record<string, unknown>)[name];
        if (!provider || typeof provider !== 'object' || Array.isArray(provider)) return false;
        const model = (provider as Record<string, unknown>).model;
        return typeof model === 'string' && model.length > 0;
    };
    if (!providerExists(value.driver_model)) {
        throw configError('driver_model must name a providers entry');
    }
    if (value.modality_assist_model !== undefined &&
        !providerExists(value.modality_assist_model)) {
        throw configError('modality_assist_model must name a providers entry');
    }
    for (const key of ['client', 'persistence']) {
        if (!isMap(doc.get(key, true))) throw configError(`${key} must be a mapping`);
    }
    for (const key of ['state', 'memory']) {
        try { persistenceChild('/session', (value.persistence as Record<string, unknown>)[key] ?? key, key); }
        catch { throw configError(`persistence.${key} must be a relative child directory without ..`); }
    }
    for (const path of [['security'], ['security', 'confirmation'], ['hub_remote_call']]) {
        if (doc.hasIn(path) && !isMap(doc.getIn(path, true))) throw configError(`${path.join('.')} must be a mapping`);
    }
    for (const path of [['client', 'endpoint'], ['security', 'confirmation', 'endpoint'], ['hub_remote_call', 'endpoint']]) {
        const endpoint = doc.getIn(path);
        if (endpoint === undefined && !doc.hasIn(path.slice(0, -1))) continue;
        if (typeof endpoint !== 'string' || !endpoint) throw configError(`${path.join('.')} must be a string`);
        const placeholder = `{{hub.${path[0] === 'client' ? 'events' : path[0] === 'security' ? 'confirm' : 'tools'}_endpoint}}`;
        if (endpoint !== placeholder) {
            try {
                const url = new URL(endpoint);
                if (!['ws:', 'wss:'].includes(url.protocol)) throw new Error();
            } catch { throw configError(`${path.join('.')} must be a WebSocket URL or its Hub placeholder`); }
        }
    }
    return doc;
}

/** Validate launcher fields using existing runtime validation, without accepting
 * arbitrary Hub settings in a launch profile. Unknown worker YAML stays intact. */
export function launchDocument(text: string, config: HubConfig): LaunchDocument {
    let value: LaunchDocument;
    try { value = parseConfigText(text, 'launch configuration') as unknown as LaunchDocument; }
    catch (error) {
        const detail = error instanceof Error ? error.message : '';
        const position = /line \d+ column \d+|position \d+/.exec(detail)?.[0];
        throw configError(`Invalid launch JSONC${position ? ` at ${position}` : ''}; check syntax and balanced braces`);
    }
    if (value.worker !== undefined && (!value.worker || typeof value.worker !== 'object' || Array.isArray(value.worker))) {
        throw configError('worker must be a mapping');
    }
    for (const key of Object.keys(value)) {
        if (!['launcher', 'worker', 'env', 'endpoints'].includes(key)) throw configError(`Unknown launch field: ${key}`);
    }
    const allowed = ['bin', 'args', 'threads', 'connectHost', 'stopTimeoutMs', 'sigtermGraceMs', 'sigkillGraceMs'] as const;
    for (const key of Object.keys(value.worker ?? {})) {
        if (!(allowed as readonly string[]).includes(key)) throw configError(`Unknown launch worker field: ${key}`);
    }
    if (!value.launcher || typeof value.launcher !== 'object' || Array.isArray(value.launcher)) throw configError('launcher is required');
    if (!['command', 'simplex-worker'].includes(value.launcher.kind)) throw configError('launcher.kind is required');
    if (value.launcher.kind === 'simplex-worker' && !value.worker?.bin) throw configError('worker.bin is required for simplex-worker');
    // Library defaults are independent of legacy Hub deployment settings.
    // Otherwise an omitted option would change meaning when the Hub restarts
    // with a different global worker/launcher configuration.
    const defaults = defaultConfig();
    const workerDefaults = Object.fromEntries(allowed.map(key => [key, defaults.worker[key]]));
    value = { ...value, launcher: mergeConfig(defaults.launcher, value.launcher),
        worker: { ...workerDefaults, ...value.worker } };
    try { validateConfig(mergeConfig(config, { launcher: value.launcher, worker: value.worker })); }
    catch (error) {
        if (error instanceof ConfigError) throw configError(error.message);
        throw configError('Invalid launch configuration fields');
    }
    for (const key of ['command', 'args'] as const) {
        if (value.launcher[key] !== undefined && (!Array.isArray(value.launcher[key]) ||
            value.launcher[key].some(item => typeof item !== 'string'))) throw configError(`launcher.${key} must contain strings`);
    }
    if (value.env !== undefined && (!value.env || typeof value.env !== 'object' || Array.isArray(value.env) ||
        Object.values(value.env).some(item => typeof item !== 'string'))) throw configError('env must map names to strings');
    if (value.endpoints !== undefined) {
        if (!value.endpoints || typeof value.endpoints !== 'object' || Array.isArray(value.endpoints)) throw configError('endpoints must be a mapping');
        for (const [key, address] of Object.entries(value.endpoints)) {
            if (!['events', 'confirm', 'tools'].includes(key)) throw configError(`Unknown endpoint: ${key}`);
            let url: URL;
            try { url = new URL(address); }
            catch { throw configError(`Invalid ${key} endpoint origin`); }
            if (!['ws:', 'wss:'].includes(url.protocol) || url.search || url.hash || url.username || url.password) {
                throw configError('Endpoint origins must be ws:// or wss:// URLs without query, fragment or credentials');
            }
        }
    }
    return value;
}

/** One process owns this store. Synchronous revision checks and publication
 * serialize panel edits without holding an asynchronous lock or losing text. */
export class ConfigurationStore {
    readonly config: HubConfig;

    constructor(config: HubConfig) {
        this.config = config;
        for (const kind of ['launch', 'worker'] as const) mkdirSync(this.directory(kind), { recursive: true, mode: 0o700 });
        if (!existsSync(this.path('launch', 'local'))) this.save('launch', 'local', this.template('launch'), null);
        if (!existsSync(this.path('worker', 'default'))) this.save('worker', 'default', this.template('worker'), null);
    }

    directory(kind: ConfigKind): string { return join(this.config.dataDir, 'configs', kind); }

    path(kind: ConfigKind, id: string): string {
        if (!['launch', 'worker'].includes(kind) || !/^[A-Za-z0-9_-]{1,128}$/.test(id)) throw configError('Invalid configuration kind or ID');
        return join(this.directory(kind), `${id}.${kind === 'launch' ? 'jsonc' : 'yaml'}`);
    }

    list(kind: ConfigKind): string[] {
        this.path(kind, 'check');
        const suffix = kind === 'launch' ? '.jsonc' : '.yaml';
        return readdirSync(this.directory(kind)).filter(name => name.endsWith(suffix) && lstatSync(join(this.directory(kind), name)).isFile())
            .map(name => name.slice(0, -suffix.length)).filter(id => /^[A-Za-z0-9_-]{1,128}$/.test(id)).sort();
    }

    /** Read source only on demand; list responses never include credentials. */
    read(kind: ConfigKind, id: string): ConfigFile {
        const path = this.path(kind, id);
        if (!existsSync(path)) throw configError('Configuration not found', 404);
        const stat = lstatSync(path);
        if (!stat.isFile()) throw configError('Configuration must be a regular file');
        if (stat.size > 1024 * 1024) throw configError('Configuration exceeds 1 MiB', 413);
        const text = readFileSync(path, 'utf8');
        return { kind, id, text, revision: createHash('sha256').update(text).digest('hex') };
    }

    validate(kind: ConfigKind, text: string): void {
        if (typeof text !== 'string' || Buffer.byteLength(text) > 1024 * 1024) throw configError('Configuration must be text, at most 1 MiB');
        if (kind === 'launch') launchDocument(text, this.config);
        else if (kind === 'worker') workerDocument(text);
        else throw configError('Invalid configuration kind');
    }

    /** null creates a new file; an existing file requires its current revision. */
    save(kind: ConfigKind, id: string, text: string, revision: string | null): ConfigFile {
        const path = this.path(kind, id);
        const previous = existsSync(path) ? this.read(kind, id) : null;
        if ((previous?.revision ?? null) !== revision) throw configError('Configuration changed; reload before saving', 409);
        this.validate(kind, text);
        writeConfigFile(path, text);
        return this.read(kind, id);
    }

    remove(kind: ConfigKind, id: string, revision: string): void {
        if (this.read(kind, id).revision !== revision) throw configError('Configuration changed; reload before deleting', 409);
        rmSync(this.path(kind, id));
    }

    /** Templates are source files, optionally seeded from the current deployment.
     * No session token is stored here; the live addresses are bound at startup. */
    template(kind: ConfigKind, source: 'default' | 'deployment' = 'default'): string {
        if (source === 'deployment' && kind === 'launch') {
            const worker = this.config.worker;
            return '// Copied from the current Hub deployment. Edit before saving.\n' + JSON.stringify({
                launcher: this.config.launcher,
                worker: {
                    bin: worker.bin, args: worker.args, threads: worker.threads,
                    connectHost: worker.connectHost, stopTimeoutMs: worker.stopTimeoutMs,
                    sigtermGraceMs: worker.sigtermGraceMs, sigkillGraceMs: worker.sigkillGraceMs,
                },
                env: {}, endpoints: {},
            }, null, 2) + '\n';
        }
        if (kind === 'launch') return readFileSync(join(hubRoot, 'schemas', 'local.jsonc'), 'utf8');
        const doc = workerDocument(readFileSync(join(hubRoot, 'schemas', 'worker.yaml'), 'utf8'));
        if (source === 'deployment') {
            const rendered = renderSessionConfig({
                config: this.config,
                sessionId: 'TEMPLATE',
                rawSpec: this.config.mock.enabled ? { provider: this.config.mock.profile } : {},
                endpoints: { events: '{{hub.events_endpoint}}', confirm: '{{hub.confirm_endpoint}}', tools: '{{hub.tools_endpoint}}' },
            }).document;
            for (const [key, value] of Object.entries(rendered)) doc.set(key, doc.createNode(value));
        }
        doc.setIn(['client', 'endpoint'], '{{hub.events_endpoint}}');
        doc.setIn(['security', 'confirmation', 'endpoint'], '{{hub.confirm_endpoint}}');
        if (source === 'default') {
            doc.set('hub_remote_call', { endpoint: '{{hub.tools_endpoint}}', timeout_ms: 120000 });
            doc.set('modality_assist_model', 'deepseek');
        }
        doc.setIn(['persistence', 'directory'], '{{session.directory}}');
        return doc.toString();
    }
}
