/** Session snapshots decouple existing conversations from later library edits. */
import { existsSync, mkdirSync, readFileSync, renameSync, rmSync } from 'node:fs';
import { join, resolve, isAbsolute } from 'node:path';
import { randomUUID } from 'node:crypto';
import { mergeConfig } from '../config.ts';
import type { HubConfig } from '../config.ts';
import type { WorkerEndpoints } from '../launch/invocation.ts';
import { ConfigurationStore, launchDocument, workerDocument, writeConfigFile, configError } from './store.ts';
import type { LaunchDocument } from './store.ts';

import type { ConfigSelection } from '../../shared/configurations.ts';
export type { ConfigSelection } from '../../shared/configurations.ts';

/** Both selectors are required together. Legacy sessions may omit both. */
export function selection(raw: unknown): ConfigSelection | null {
    const value = raw as Partial<ConfigSelection> | null;
    if (!value || (value.launchConfig === undefined && value.workerConfig === undefined)) return null;
    if (typeof value.launchConfig !== 'string' || typeof value.workerConfig !== 'string') {
        throw configError('Select both a launch configuration and a worker configuration');
    }
    return { launchConfig: value.launchConfig, workerConfig: value.workerConfig };
}

/** Validate both files before publication. Directory replacement is synchronous;
 * on a failed rename the old snapshot is restored before the request returns. */
export function snapshotConfigs(store: ConfigurationStore, sessionId: string, selected: ConfigSelection, replace = false): void {
    if (!/^[A-Za-z0-9_-]{1,128}$/.test(sessionId)) throw configError('Invalid session ID');
    const launch = store.read('launch', selected.launchConfig);
    const worker = store.read('worker', selected.workerConfig);
    store.validate('launch', launch.text);
    store.validate('worker', worker.text);
    const root = join(store.config.dataDir, 'sessions', sessionId);
    const destination = join(root, 'config');
    if (existsSync(destination) && !replace) throw configError('Session configuration already exists', 409);
    mkdirSync(root, { recursive: true });
    const temporary = join(root, `.config-${randomUUID()}`);
    const backup = join(root, `.config-backup-${randomUUID()}`);
    mkdirSync(temporary, { mode: 0o700 });
    let moved = false;
    try {
        writeConfigFile(join(temporary, 'launch.jsonc'), launch.text);
        const doc = workerDocument(worker.text);
        doc.setIn(['persistence', 'directory'], root);
        writeConfigFile(join(temporary, 'config.yaml'), doc.toString());
        writeConfigFile(join(temporary, 'source.json'), JSON.stringify({ ...selected, launchRevision: launch.revision, workerRevision: worker.revision }));
        if (existsSync(destination)) { renameSync(destination, backup); moved = true; }
        try { renameSync(temporary, destination); }
        catch (error) { if (moved) renameSync(backup, destination); throw error; }
    } finally {
        rmSync(temporary, { recursive: true, force: true });
    }
    if (moved) rmSync(backup, { recursive: true, force: true });
}

/** The published snapshot is authoritative for its configuration selectors.
 * A missing or invalid source file belongs to legacy/incomplete snapshots and
 * leaves the persisted spec unchanged. */
export function snapshotSelection(config: HubConfig, id: string): ConfigSelection | null {
    const path = join(config.dataDir, 'sessions', id, 'config', 'source.json');
    if (!existsSync(path)) return null;
    try {
        const source: unknown = JSON.parse(readFileSync(path, 'utf8'));
        return selection(source);
    } catch {
        return null;
    }
}

export function sessionLaunch(config: HubConfig, id: string): { config: HubConfig; launch: LaunchDocument } | null {
    const path = join(config.dataDir, 'sessions', id, 'config', 'launch.jsonc');
    if (!existsSync(path)) return null;
    const launch = launchDocument(readFileSync(path, 'utf8'), config);
    const resolved = mergeConfig(config, { launcher: launch.launcher, worker: launch.worker ?? {} });
    const directory = join(config.dataDir, 'sessions', id, 'config');
    if (resolved.launcher.cwd) resolved.launcher.cwd = resolve(directory, resolved.launcher.cwd);
    if (launch.worker?.bin && !isAbsolute(launch.worker.bin)) resolved.worker.bin = resolve(directory, launch.worker.bin);
    return { config: resolved, launch };
}

/** Replace only origin/prefix, retaining Hub-issued session identity and token.
 * Explicit origins handle containers and reverse proxies without trusting Host. */
export function launchEndpoints(base: WorkerEndpoints, launch: LaunchDocument): WorkerEndpoints {
    const result = { ...base };
    for (const key of ['events', 'confirm', 'tools'] as const) {
        const url = new URL(base[key]);
        if (launch.worker?.connectHost) url.hostname = launch.worker.connectHost;
        const origin = launch.endpoints?.[key];
        if (origin) {
            const advertised = new URL(origin);
            advertised.pathname = advertised.pathname.replace(/\/$/, '') + url.pathname;
            advertised.search = url.search;
            result[key] = advertised.toString();
        } else result[key] = url.toString();
    }
    return result;
}
