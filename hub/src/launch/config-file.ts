/**
 * Durable per-session configuration. Only hub-owned paths and live connection
 * addresses are refreshed on restart; operator settings and YAML comments stay.
 */
import { randomUUID } from 'node:crypto';
import { chmodSync, chownSync, existsSync, lstatSync, mkdirSync, readFileSync, realpathSync, renameSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { dirname, isAbsolute, join, normalize, parse, sep } from 'node:path';
import { isMap, parseDocument } from 'yaml';
import type { Document } from 'yaml';
import { renderSessionConfig, sessionDir, workerConfigPath } from './config-render.ts';
import type { RenderSessionConfigOptions } from './config-render.ts';
import { normalizeSpec } from './spec.ts';
import type { NormalizedSpec } from './spec.ts';
import { checkPromptFile, type HubConfig } from '../config.ts';

/** Same lexical child-directory rule as load::parse_configuration. */
export function persistenceChild(root: string, value: unknown, key: string): string {
    if (typeof value !== 'string' || !value || value.includes('\0')
        || isAbsolute(value) || parse(value).root !== ''
        || value.split(sep === '\\' ? /[\\/]/ : /\//).includes('..')) {
        throw new Error(`persistence.${key} must be a nonempty relative directory without ..`);
    }
    return normalize(join(root, value));
}

/** Parse real YAML as well as the JSON subset; never repair a malformed file. */
function readDocument(path: string): Document {
    const document = parseDocument(readFileSync(path, 'utf8'));
    if (document.errors.length || !isMap(document.contents)) {
        // Parser diagnostics may contain credentials from the input line.
        throw new Error(`invalid YAML mapping in ${path}`);
    }
    // Bound alias expansion before touching the document.
    document.toJS({ maxAliasCount: 100 });
    return document;
}

/** Existing parent nodes must be mappings; refuse to silently replace settings. */
function mapping(document: Document, path: string[]): void {
    for (let count = 1; count <= path.length; count += 1) {
        const prefix = path.slice(0, count);
        if (!document.hasIn(prefix)) document.setIn(prefix, document.createNode({}));
        if (!isMap(document.getIn(prefix, true))) {
            throw new Error(`${prefix.join('.')} must be a mapping`);
        }
    }
}

/** Publish a complete file, keeping the original intact if preparation fails. */
function writeDocument(path: string, text: string): void {
    mkdirSync(dirname(path), { recursive: true });
    const temporary = `${path}.${randomUUID()}.tmp`;
    try {
        const previous = existsSync(path) ? statSync(path) : null;
        writeFileSync(temporary, text, { flag: 'wx', mode: 0o600 });
        if (previous) {
            // A refresh must not revoke access granted to a worker running
            // under a compatible, but different, uid or group.
            const created = statSync(temporary);
            if (created.uid !== previous.uid || created.gid !== previous.gid) {
                chownSync(temporary, previous.uid, previous.gid);
            }
            chmodSync(temporary, previous.mode & 0o777);
        }
        renameSync(temporary, path);
    } finally {
        rmSync(temporary, { force: true });
    }
}

/**
 * Create once, then refresh only the root, event/confirmation/tool URLs (including
 * their session tokens), and the active mock profile's dynamic base URL.
 * Existing model credentials and unknown configuration keys remain untouched.
 * Launch-only spec fields still control threads, environment and extra args.
 */
export function prepareSessionConfig(options: RenderSessionConfigOptions & { config: HubConfig }): {
    spec: NormalizedSpec;
    document: unknown;
    /** The removed default prompt a saved configuration pinned, if migrated. */
    migratedPrompt?: string;
} {
    const { config, sessionId, rawSpec, endpoints, mock } = options;
    const path = workerConfigPath(config, sessionId);
    if (!existsSync(path)) {
        const rendered = renderSessionConfig(options);
        writeDocument(path, `${JSON.stringify(rendered.document, null, 2)}\n`);
        return rendered;
    }

    const document = readDocument(path);
    // Migrate only a verified missing historical asset in the selected local
    // installation. An older worker, a retained customized legacy file, and an
    // unknown/remote installation must keep their explicit selections.
    const worker = document.get('worker', true);
    const savedPrompt = isMap(worker) ? worker.get('system_prompt_file') : undefined;
    const migratedPrompt = canMigratePrompt(savedPrompt, config) ? savedPrompt as string : null;
    if (migratedPrompt !== null) document.deleteIn(['worker', 'system_prompt_file']);
    // A persisted configuration no longer depends on its original hub profile
    // still existing. Only the launch parameters are taken from the spec.
    const raw = rawSpec && typeof rawSpec === 'object' && !Array.isArray(rawSpec)
        ? rawSpec as Record<string, unknown> : {};
    const spec = normalizeSpec(config as HubConfig, {
        threads: raw.threads, env: raw.env, extraArgs: raw.extraArgs,
    });
    const root = sessionDir(config, sessionId);
    mapping(document, ['persistence']);
    for (const key of ['state', 'memory']) {
        const child = document.hasIn(['persistence', key])
            ? document.getIn(['persistence', key]) : key;
        persistenceChild(root, child, key);
    }
    mapping(document, ['client']);
    // Omission is an operator choice to disable this optional toolset.
    if (document.has('hub_remote_call')) {
        mapping(document, ['hub_remote_call']);
        document.setIn(['hub_remote_call', 'endpoint'], endpoints.tools);
        if (!document.hasIn(['hub_remote_call', 'timeout_ms'])) {
            document.setIn(['hub_remote_call', 'timeout_ms'], config.toolRequests.timeoutMs);
        }
    }
    if (document.hasIn(['security', 'confirmation'])) {
        mapping(document, ['security', 'confirmation']);
        document.setIn(['security', 'confirmation', 'endpoint'], endpoints.confirm);
    }
    document.setIn(['persistence', 'directory'], root);
    document.setIn(['client', 'endpoint'], endpoints.events);
    if (mock?.baseUrl && document.get('driver_model') === config.mock.profile) {
        mapping(document, ['providers', config.mock.profile, 'endpoint']);
        document.setIn(['providers', config.mock.profile, 'endpoint', 'base_url'], mock.baseUrl);
    }
    const result = document.toJS({ maxAliasCount: 100 });
    applyConfigurationSpec(spec, result);
    writeDocument(path, document.toString());
    return { spec, document: result, ...(migratedPrompt === null ? {} : { migratedPrompt }) };
}

/**
 * Resolve only the built-in direct launcher's executable, including symlinks.
 * Other launchers may select a different machine or executable; never inspect
 * their paths as though they belonged to the Hub filesystem.
 */
function localInstallation(config: HubConfig): string | null {
    if (config.launcher.kind !== 'simplex-worker') return null;
    const bin = config.worker.bin;
    if (typeof bin !== 'string' || !/[\\/]/.test(bin)) return null;
    try { return dirname(realpathSync(bin)); } catch { return null; }
}

/** Only ENOENT proves absence; permission, I/O and non-file errors preserve the selection. */
function missingFile(path: string): boolean {
    try { lstatSync(path); return false; }
    catch (error) { return (error as NodeJS.ErrnoException).code === 'ENOENT'; }
}

/**
 * The two historical filenames alone are not proof of an obsolete setting.
 * Require a missing legacy asset and a regular replacement in the same known
 * installation. Keep even dangling legacy symlinks as explicit operator choices.
 */
function canMigratePrompt(value: unknown, config: HubConfig): boolean {
    if (value !== 'prompts/coding_agent.yaml' && value !== 'prompts/general_agent.yaml') return false;
    const installation = localInstallation(config);
    if (installation === null || !missingFile(join(installation, value))) return false;
    try { return statSync(join(installation, 'prompts/default.yaml')).isFile(); }
    catch { return false; }
}

/**
 * Name the remedy when a session pins a prompt file the deployed worker cannot
 * see. The worker reports only `cannot open file`, which is easy to mistake for
 * a broken installation.
 *
 * Returns null when the selection is inherited, the file is present, or this
 * host cannot know the worker's installation directory: a `bin` resolved
 * through PATH has none, and a managed or containerized worker owns a different
 * filesystem.
 */
export function missingPromptDiagnostic(spec: NormalizedSpec, config: HubConfig): string | null {
    if (spec.systemPromptFile === undefined) return null;
    // The worker resolves its installation through the real executable path
    // (`std::filesystem::canonical`), so a symlinked launcher must not point the
    // diagnostic at the link's directory. A missing binary has no installation
    // to inspect; the spawn reports that separately.
    const installation = localInstallation(config);
    if (installation === null) return null;
    const installed = join(installation, spec.systemPromptFile);
    if (!missingFile(installed)) return null;
    return `system prompt '${spec.systemPromptFile}' is missing from the worker installation `
        + `'${installation}'; remove worker.system_prompt_file from the session configuration `
        + `to inherit the deployed default (prompts/default.yaml in current releases), `
        + `or install the file at '${installed}'`;
}

/** Project public model/runtime metadata from an effective configuration without opening files. */
export function applyConfigurationSpec(spec: NormalizedSpec, result: any): void {
    // Session descriptions should continue to identify the model in the file,
    // even if that profile has been removed from the hub defaults.
    if (typeof result.driver_model === 'string') {
        spec.provider = result.driver_model;
        spec.model = result.providers?.[spec.provider]?.model ?? '';
    }
    // The saved worker config is authoritative for the assistant as well.
    // In particular, an operator may have removed or retargeted this role
    // since the session spec was last saved in hub.json.
    const assist = result.modality_assist_model;
    if (assist === undefined) {
        spec.modalityAssistProvider = null;
    } else if (typeof assist === 'string' && assist.length > 0
        && result.providers?.[assist]
        && typeof result.providers[assist] === 'object'
        && !Array.isArray(result.providers[assist])) {
        spec.modalityAssistProvider = assist;
    } else {
        throw new Error('modality_assist_model must name a providers mapping');
    }
    spec.maxExchanges = result.worker?.max_exchanges ?? spec.maxExchanges;
    spec.autoCompactThreshold = result.worker?.auto_compact_threshold ?? 0;
    spec.maxAutoCompactions = result.worker?.max_auto_compactions ?? 5;
    if (!Number.isSafeInteger(spec.autoCompactThreshold) || spec.autoCompactThreshold < 0
        || spec.autoCompactThreshold > 2147483647
        || !Number.isSafeInteger(spec.maxAutoCompactions) || spec.maxAutoCompactions <= 0
        || spec.maxAutoCompactions > 2147483647) {
        throw new Error('invalid auto compact threshold or attempt budget');
    }
    spec.eventCapacity = result.worker?.event_capacity ?? spec.eventCapacity;
    // A saved omission means inherit the worker default, not today's Hub override.
    if (result.worker?.system_prompt_file === undefined) {
        delete spec.systemPromptFile;
    } else {
        spec.systemPromptFile = checkPromptFile(result.worker.system_prompt_file, 'worker.system_prompt_file');
    }
    spec.workspace = result.worker?.environment?.workspace ?? '';
    spec.platform = result.worker?.environment?.platform ?? '';
    spec.software = result.worker?.environment?.software ?? [];
    const backend = result.persistence?.backend;
    if (backend !== undefined && backend !== 'json' && backend !== 'sqlite') {
        throw new Error('persistence.backend must be json or sqlite');
    }
    const format = result.persistence?.format;
    if (format !== undefined && (format !== 'json' || backend === 'sqlite')) {
        throw new Error('conflicting or unsupported persistence.format');
    }
    spec.persistence = {
        // Saved configuration remains authoritative. Legacy configurations select
        // JSON; do not replace their choice with current Hub defaults.
        backend: result.persistence?.backend ?? (result.persistence?.format === 'json' ? 'json' : undefined),
        enabled: result.persistence?.enabled ?? true,
    };
    spec.restore = result.persistence?.restore ?? 'if_present';
}

/** Resolve snapshots from saved config, including operator-selected state subdirectories. */
export function sessionStateDirectory(config: { dataDir: string }, sessionId: string): string {
    const path = workerConfigPath(config, sessionId);
    const document = existsSync(path) ? readDocument(path) : null;
    if (document?.has('persistence') && !isMap(document.get('persistence', true))) {
        throw new Error('persistence must be a mapping');
    }
    const state = document?.hasIn(['persistence', 'state'])
        ? document.getIn(['persistence', 'state']) : 'state';
    return persistenceChild(sessionDir(config, sessionId), state, 'state');
}

/** Legacy local inspection must honor saved backend policy instead of exposing
 * a stale JSON file beside a selected SQLite store. This does not read SQL. */
export function sessionStateBackend(config: { dataDir: string }, sessionId: string): 'json' | 'sqlite' {
    const path = workerConfigPath(config, sessionId);
    const document = existsSync(path) ? readDocument(path) : null;
    const explicit = document?.getIn(['persistence', 'backend']);
    const format = document?.getIn(['persistence', 'format']);
    if (explicit !== undefined && explicit !== 'json' && explicit !== 'sqlite') throw new Error('invalid persistence backend');
    if (format !== undefined && (format !== 'json' || explicit === 'sqlite')) throw new Error('conflicting persistence format');
    const directory = sessionStateDirectory(config, sessionId);
    const json = existsSync(join(directory, 'state.json'));
    const sqlite = existsSync(join(directory, 'state.sqlite3'));
    const selected = explicit ?? (format === 'json' ? 'json' : undefined);
    // Both stores present: any selection would abandon the other history, so
    // inspection refuses the directory exactly like backend selection does.
    if (json && sqlite) throw new Error('both JSON and SQLite stores exist; remove or migrate one before inspection');
    if ((selected === 'json' && sqlite) || (selected === 'sqlite' && json)) {
        throw new Error('selected persistence backend conflicts with existing history');
    }
    return selected ?? (json ? 'json' : 'sqlite');
}
