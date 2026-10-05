/** Installer metadata is local bookkeeping, not proof of release authenticity. */
import { access, lstat, readFile, readdir, stat } from 'node:fs/promises';
import { constants } from 'node:fs';
import { join } from 'node:path';

export const METADATA = '.simplex-worker-install.json';
export interface InstalledWorker {
    source: 'github';
    version: string;
    installedAt: string;
}

/** Accept the repository's stable three-component release convention only. */
export function normalizeVersion(value: string): string {
    const version = value.replace(/^v/, '');
    if (!/^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/.test(version)) {
        throw new Error(`Invalid stable release version: ${value}; expected vMAJOR.MINOR.PATCH`);
    }
    return version;
}

/** BigInt avoids lexical ordering and precision loss in numeric components. */
export function compareVersions(left: string, right: string): number {
    const a = normalizeVersion(left).split('.').map(BigInt);
    const b = normalizeVersion(right).split('.').map(BigInt);
    for (let index = 0; index < 3; index++) {
        if (a[index]! < b[index]!) return -1;
        if (a[index]! > b[index]!) return 1;
    }
    return 0;
}

/** Unknown/manual or malformed metadata never implies ownership or a version. */
export async function readInstalled(directory: string): Promise<InstalledWorker | null> {
    try {
        const path = join(directory, METADATA);
        const stat = await lstat(path);
        if (!stat.isFile() || stat.size > 4096) return null;
        const value = JSON.parse(await readFile(path, 'utf8')) as InstalledWorker;
        if (value.source !== 'github' || typeof value.version !== 'string'
            || typeof value.installedAt !== 'string') return null;
        normalizeVersion(value.version);
        return value;
    } catch {
        return null;
    }
}

/** Required payload shared by fresh preparation and a current-version no-op. */
export async function validateTree(directory: string): Promise<void> {
    for (const file of [
        'bin/simplex', 'bin/simplex_worker', 'bin/config.example.yaml',
        'bin/prompts/coding_agent.yaml', 'bin/prompts/operations/compact.yaml',
        'bin/schemas/process/skill.yaml', 'bin/schemas/process/run_command.yaml',
        'bin/schemas/process/spawn_process.yaml', 'bin/schemas/process/read_process.yaml',
        'bin/schemas/process/send_process.yaml', 'bin/schemas/process/poll_process.yaml',
        'LICENSE', 'README.md',
    ]) {
        const stat = await lstat(join(directory, file));
        if (!stat.isFile() || stat.size === 0) throw new Error(`Missing required worker file: ${file}`);
    }
    for (const executable of ['bin/simplex', 'bin/simplex_worker']) {
        const path = join(directory, executable);
        if (((await lstat(path)).mode & 0o111) === 0) throw new Error(`Not executable: ${executable}`);
        await access(path, constants.X_OK);
    }
    for (const folder of ['lib', 'bin/plugins/llm', 'third_party_licenses']) {
        if (!(await lstat(join(directory, folder))).isDirectory()) {
            throw new Error(`Missing required worker directory: ${folder}`);
        }
    }
    // At least one provider must be present; old releases need not contain Qwen.
    const plugins = await readdir(join(directory, 'bin/plugins/llm'));
    const providers = plugins.filter(name => /^libllm_.+\.so$/.test(name));
    if (providers.length === 0) {
        throw new Error('Worker installation contains no model-provider plugin');
    }
    for (const provider of providers) {
        const info = await stat(join(directory, 'bin/plugins/llm', provider));
        if (!info.isFile() || info.size === 0) throw new Error(`Invalid model-provider plugin: ${provider}`);
    }
    if ((await readdir(join(directory, 'lib'))).length === 0) throw new Error('Worker runtime library directory is empty');
}

/** Independent authorization flags must not implicitly authorize each other. */
export function installationAction(installed: InstalledWorker | null, target: string,
    flags: { reinstall: boolean; allowDowngrade: boolean }): 'install' | 'current' {
    if (!installed) return 'install';
    const order = compareVersions(target, installed.version);
    if (order < 0 && !flags.allowDowngrade) {
        throw new Error(`Downgrade from ${installed.version} to ${target} requires --allow-downgrade`);
    }
    return order === 0 && !flags.reinstall ? 'current' : 'install';
}
