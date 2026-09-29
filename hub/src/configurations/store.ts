/** Durable, editable configuration library. Files retain comments and are never
 * expanded in place: session-specific values belong in session snapshots. */
import { createHash, randomUUID } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, readdirSync, renameSync, rmSync, writeFileSync, lstatSync } from 'node:fs';
import { join } from 'node:path';
import { isMap, parseDocument } from 'yaml';
import { hubRoot, mergeConfig, parseConfigText, validateConfig } from '../config.ts';
import type { HubConfig } from '../config.ts';

export type ConfigKind = 'launch' | 'worker';
export interface ConfigFile { id: string; kind: ConfigKind; text: string; revision: string }
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

export function workerDocument(text: string) {
    const doc = parseDocument(text);
    if (doc.errors.length) {
        const error = doc.errors[0];
        throw configError(`Invalid YAML at line ${error?.linePos?.[0]?.line ?? '?'}: ${error?.code}`);
    }
    if (!isMap(doc.contents)) throw configError('Worker configuration must be a YAML mapping');
    const value = doc.toJS({ maxAliasCount: 100 });
    if (typeof value.driver_model !== 'string' || !value.providers?.[value.driver_model]) {
        throw configError('driver_model must name a providers entry');
    }
    if (value.modality_assist_model !== undefined &&
        (typeof value.modality_assist_model !== 'string' || !value.providers?.[value.modality_assist_model])) {
        throw configError('modality_assist_model must name a providers entry');
    }
    for (const key of ['client', 'persistence']) {
        if (!isMap(doc.get(key, true))) throw configError(`${key} must be a mapping`);
    }
    return doc;
}

/** Validate launcher fields using existing runtime validation, without accepting
 * arbitrary Hub settings in a launch profile. Unknown worker YAML stays intact. */
export function launchDocument(text: string, config: HubConfig): LaunchDocument {
    const value = parseConfigText(text, 'launch configuration') as unknown as LaunchDocument;
    for (const key of Object.keys(value)) {
        if (!['launcher', 'worker', 'env', 'endpoints'].includes(key)) throw configError(`Unknown launch field: ${key}`);
    }
    const allowed = ['bin', 'args', 'threads', 'connectHost', 'stopTimeoutMs', 'sigtermGraceMs', 'sigkillGraceMs'];
    for (const key of Object.keys(value.worker ?? {})) {
        if (!allowed.includes(key)) throw configError(`Unknown launch worker field: ${key}`);
    }
    if (!value.launcher || typeof value.launcher !== 'object') throw configError('launcher is required');
    validateConfig(mergeConfig(config, { launcher: value.launcher, worker: value.worker ?? {} }));
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
            const url = new URL(address);
            if (!['ws:', 'wss:'].includes(url.protocol) || url.search || url.hash || url.username || url.password) {
                throw configError('Endpoint origins must be ws:// or wss:// URLs without query, fragment or credentials');
            }
        }
    }
    return value;
}

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
        return readdirSync(this.directory(kind)).filter(name => name.endsWith(suffix))
            .map(name => name.slice(0, -suffix.length)).filter(id => /^[A-Za-z0-9_-]{1,128}$/.test(id)).sort();
    }

    read(kind: ConfigKind, id: string): ConfigFile {
        const path = this.path(kind, id);
        if (!existsSync(path)) throw configError('Configuration not found', 404);
        if (!lstatSync(path).isFile()) throw configError('Configuration must be a regular file');
        const text = readFileSync(path, 'utf8');
        return { kind, id, text, revision: createHash('sha256').update(text).digest('hex') };
    }

    validate(kind: ConfigKind, text: string): void {
        if (typeof text !== 'string' || Buffer.byteLength(text) > 1024 * 1024) throw configError('Configuration must be text, at most 1 MiB');
        if (kind === 'launch') launchDocument(text, this.config);
        else if (kind === 'worker') workerDocument(text);
        else throw configError('Invalid configuration kind');
    }

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

    template(kind: ConfigKind): string {
        if (kind === 'launch') return '// Local worker; simplex must be installed on the Hub host PATH.\n' + JSON.stringify({
            launcher: { kind: 'command', command: ['simplex', 'run', '--config', '{config}', '--session', '{session}', '--threads', '{threads}'], args: [], cwd: '', pidFile: '' },
            worker: { threads: 1, connectHost: '', stopTimeoutMs: 15000, sigtermGraceMs: 5000, sigkillGraceMs: 2000 },
            env: {}, endpoints: {},
        }, null, 2) + '\n';
        const doc = workerDocument(readFileSync(join(hubRoot, 'schemas', 'worker.yaml'), 'utf8'));
        doc.setIn(['client', 'endpoint'], '{{hub.events_endpoint}}');
        doc.setIn(['security', 'confirmation', 'endpoint'], '{{hub.confirm_endpoint}}');
        doc.set('hub_remote_call', { endpoint: '{{hub.tools_endpoint}}', timeout_ms: 120000 });
        doc.set('modality_assist_model', 'deepseek');
        doc.setIn(['persistence', 'directory'], '{{session.directory}}');
        return doc.toString();
    }
}
