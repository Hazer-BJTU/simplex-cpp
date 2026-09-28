/**
 * Durable per-session configuration. Only hub-owned paths and live connection
 * addresses are refreshed on restart; operator settings and YAML comments stay.
 */
import { randomUUID } from 'node:crypto';
import { chmodSync, chownSync, existsSync, mkdirSync, readFileSync, renameSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { dirname, isAbsolute, join, normalize, parse, sep } from 'node:path';
import { isMap, parseDocument } from 'yaml';
import type { Document } from 'yaml';
import { renderSessionConfig, sessionDir, workerConfigPath } from './config-render.ts';
import type { RenderSessionConfigOptions } from './config-render.ts';
import { normalizeSpec } from './spec.ts';
import type { NormalizedSpec } from './spec.ts';
import type { HubConfig } from '../config.ts';

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
        if (!document.hasIn(prefix)) document.setIn(prefix, {});
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
 * Create once, then refresh only the root, event/confirmation URLs (including
 * their session tokens), and the active mock profile's dynamic base URL.
 * Existing model credentials and unknown configuration keys remain untouched.
 * Launch-only spec fields still control threads, environment and extra args.
 */
export function prepareSessionConfig(options: RenderSessionConfigOptions): {
    spec: NormalizedSpec;
    document: unknown;
} {
    const { config, sessionId, rawSpec, endpoints, mock } = options;
    const path = workerConfigPath(config, sessionId);
    if (!existsSync(path)) {
        const rendered = renderSessionConfig(options);
        writeDocument(path, `${JSON.stringify(rendered.document, null, 2)}\n`);
        return rendered;
    }

    const document = readDocument(path);
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
    mapping(document, ['security', 'confirmation']);
    document.setIn(['persistence', 'directory'], root);
    document.setIn(['client', 'endpoint'], endpoints.events);
    document.setIn(['security', 'confirmation', 'endpoint'], endpoints.confirm);
    if (mock?.baseUrl && document.get('driver_model') === config.mock.profile) {
        mapping(document, ['providers', config.mock.profile, 'endpoint']);
        document.setIn(['providers', config.mock.profile, 'endpoint', 'base_url'], mock.baseUrl);
    }
    const result = document.toJS({ maxAliasCount: 100 });
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
    spec.eventCapacity = result.worker?.event_capacity ?? spec.eventCapacity;
    spec.systemPromptFile = result.worker?.system_prompt_file ?? spec.systemPromptFile;
    spec.workspace = result.worker?.environment?.workspace ?? '';
    spec.platform = result.worker?.environment?.platform ?? '';
    spec.software = result.worker?.environment?.software ?? [];
    spec.persistence = {
        enabled: result.persistence?.enabled ?? true,
        readable: result.persistence?.readable ?? false,
    };
    spec.restore = result.persistence?.restore ?? 'if_present';
    writeDocument(path, document.toString());
    return { spec, document: result };
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
