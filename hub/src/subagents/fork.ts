/** Capture startup configuration and reproduce it with fresh persistence/identity. */
import { existsSync, readFileSync } from 'node:fs';
import { join, isAbsolute, resolve, basename } from 'node:path';
import { parseDocument, isMap } from 'yaml';
import type { HubConfig } from '../config.ts';
import type { Session } from '../state/registry.ts';
import type { LaunchDocument } from '../configurations/store.ts';
import { writeConfigFile, launchDocument } from '../configurations/store.ts';
import { sessionDir } from '../launch/config-render.ts';
import { privateDirectory } from './storage.ts';
import { ToolFailure } from '../worker/tool-context.ts';

/** Normalize known path fields, without guessing the meaning of arbitrary provider values. */
function workerPaths(document: ReturnType<typeof parseDocument>, sourceDirectory: string): void {
    const resolveValue = (path: (string | number)[]): void => {
        const value = document.getIn(path);
        if (typeof value === 'string' && value && !isAbsolute(value)) {
            document.setIn(path, resolve(sourceDirectory, value));
        }
    };
    resolveValue(['worker', 'environment', 'workspace']);
    for (const prefix of [['plugins', 'providers'], ['plugins', 'extensions', 'tools'],
        ['plugins', 'extensions', 'loop_hooks']]) {
        const directories = document.toJS({ maxAliasCount: 100 });
        let section = directories;
        for (const key of prefix) section = section?.[key];
        if (Array.isArray(section?.directories)) {
            section.directories.forEach((_: unknown, i: number) => resolveValue([...prefix, 'directories', i]));
        }
        if (Array.isArray(section?.enable)) {
            section.enable.forEach((_: unknown, i: number) => {
                for (const key of ['schema_directory', 'config_file']) resolveValue([...prefix, 'enable', i, key]);
            });
        }
    }
}

/** A captured launch is independent of subsequent configuration-library edits. */
export function captureStartup(config: HubConfig, session: Session, workerText: string,
    launch: LaunchDocument): void {
    const directory = join(sessionDir(config, session.id), 'config');
    privateDirectory(directory, true);
    const doc = parseDocument(workerText);
    if (doc.errors.length || !isMap(doc.contents)) throw new Error('invalid startup worker mapping');
    workerPaths(doc, directory);
    writeConfigFile(join(directory, 'startup-worker.yaml'), doc.toString());
    writeConfigFile(join(directory, 'startup-launch.jsonc'), JSON.stringify(launch));
}

/** Initially support native workers and foreground Docker launch templates only. */
export function supportedLaunch(launch: LaunchDocument): boolean {
    if (launch.launcher.pidFile) return false;
    if (launch.launcher.kind === 'simplex-worker') {
        // Extra flags overriding identity/configuration would defeat clean-fork.
        return ![...(launch.launcher.args ?? []), ...(launch.worker?.args ?? [])]
            .some(value => /^--(?:config|session)(?:=|$)/.test(value));
    }
    const command = [...launch.launcher.command, ...launch.launcher.args];
    return basename(command[0] ?? '') === 'docker' && command[1] === 'run'
        && command.includes('--rm') && command.includes('--init')
        && command.includes('--name') && command[command.indexOf('--name') + 1]?.includes('{session}') === true
        && !command.some(value => value === '-d' || value.startsWith('--detach') || /^-[a-z]*d[a-z]*$/.test(value))
        && command.includes('{session_dir}:{session_dir}')
        && command.includes('{config}') && command.includes('{session}')
        && !command.some(value => /--(?:config|session)=/.test(value));
}

export function cleanFork(config: HubConfig, parent: Session, child: Session): void {
    const source = join(sessionDir(config, parent.id), 'config');
    privateDirectory(source);
    const workerPath = join(source, 'startup-worker.yaml');
    const launchPath = join(source, 'startup-launch.jsonc');
    if (!existsSync(workerPath) || !existsSync(launchPath) || !parent.process || !parent.lifecycleId) {
        throw new ToolFailure('unsupported_launch', 'parent has no reproducible supervised startup snapshot');
    }
    // Capture paths were validated on publication; recheck private directory before use.
    const launch = launchDocument(readFileSync(launchPath, 'utf8'), config);
    if (!supportedLaunch(launch)) {
        throw new ToolFailure('unsupported_launch', 'clean-fork requires a native worker or foreground Docker template');
    }
    const doc = parseDocument(readFileSync(workerPath, 'utf8'));
    if (doc.errors.length || !isMap(doc.contents)) throw new ToolFailure('storage_error', 'invalid startup snapshot');
    const root = sessionDir(config, child.id);
    privateDirectory(root, true);
    privateDirectory(join(root, 'config'), true);
    doc.setIn(['persistence', 'directory'], root);
    // First startup cannot require a state file copied from the parent.
    doc.setIn(['persistence', 'restore'], 'if_present');
    if (!doc.has('security')) doc.set('security', doc.createNode({}));
    if (!doc.hasIn(['security', 'confirmation'])) doc.setIn(['security', 'confirmation'], doc.createNode({}));
    if (!doc.has('hub_remote_call')) doc.set('hub_remote_call', doc.createNode({}));
    doc.setIn(['security', 'confirmation', 'endpoint'], '{{hub.confirm_endpoint}}');
    doc.setIn(['hub_remote_call', 'endpoint'], '{{hub.tools_endpoint}}');
    writeConfigFile(join(root, 'config', 'launch.jsonc'), JSON.stringify(launch));
    writeConfigFile(join(root, 'config', 'config.yaml'), doc.toString());
}

/** Resolve supported launcher paths when snapshotting their startup meaning. */
export function resolvedStartupLaunch(config: HubConfig, launch: LaunchDocument | null,
    environment: Record<string, string>): LaunchDocument {
    return {
        ...(launch ?? {}), launcher: { ...config.launcher },
        worker: {
            bin: config.worker.bin, args: [...config.worker.args], threads: config.worker.threads,
            connectHost: config.worker.connectHost, stopTimeoutMs: config.worker.stopTimeoutMs,
            sigtermGraceMs: config.worker.sigtermGraceMs, sigkillGraceMs: config.worker.sigkillGraceMs,
        },
        env: { ...environment },
    };
}
