/** Standalone worker installer; does not load Hub configuration or start listeners. */
import { homedir } from 'node:os';
import { join } from 'node:path';
import { mkdir, readdir } from 'node:fs/promises';
import { createInterface } from 'node:readline';
import type { Readable, Writable } from 'node:stream';
import { prepareArchive } from './archive.ts';
import { pathCommand, bashQuote, updateBashrc } from './bashrc.ts';
import { atomicWrite, exists } from './files.ts';
import { smokeCheck, validateHost } from './host.ts';
import { resolveRelease } from './source.ts';
import type { Fetch, WorkerRelease } from './source.ts';
import { destinationPath, InstallationTransaction } from './transaction.ts';
import { METADATA, installationAction, normalizeVersion, readInstalled, validateTree } from './version.ts';

export const INSTALL_USAGE = `Usage: simplex-hub install-worker [options]

Download and install a worker on this host without starting the Hub.

Options:
  --source <source>       release source (default: github; only github is supported)
  --version <vX.Y.Z>      published stable version (default: latest stable release)
  --directory <dir>       installation root (default: ~/.simplex/worker)
  --update-path           update the managed PATH block in ~/.bashrc
  --no-update-path        leave ~/.bashrc unchanged
  --reinstall             replace a current installation after validating the release
  --allow-downgrade       authorize an older known version
  --overwrite             authorize replacement of an unknown non-empty directory
  -h, --help              show this message

Replacement removes custom/stale files inside the installation root.
Stop workers using this installation before replacing it. Keep session data elsewhere.
Linux x86_64, glibc >= 2.34, Bash, util-linux flock, ldd and host OpenSSL 3 are required.
PATH changes apply to new shells; restart the Hub from an updated shell.
Directories containing ":" can be installed but cannot be added to PATH; use an absolute command.
Downloads install executable code/plugins. SHA256 detects corruption, not publisher identity.
`;

export interface InstallOptions {
    source: string;
    version?: string;
    directory: string;
    updatePath?: boolean;
    reinstall: boolean;
    allowDowngrade: boolean;
    overwrite: boolean;
    help: boolean;
}

/** Installation decisions have distinct flags; unknown server flags are rejected. */
export function parseInstallArguments(argv: string[], home = homedir()): InstallOptions {
    const options: InstallOptions = { source: 'github', directory: join(home, '.simplex/worker'),
        reinstall: false, allowDowngrade: false, overwrite: false, help: false };
    for (let index = 0; index < argv.length; index++) {
        const argument = argv[index];
        switch (argument) {
            case '--source': case '--version': case '--directory': {
                const value = argv[++index];
                if (!value || value.startsWith('--')) throw new Error(`${argument} requires a value`);
                if (argument === '--source') options.source = value;
                else if (argument === '--version') options.version = normalizeVersion(value);
                else options.directory = value;
                break;
            }
            case '--update-path': case '--no-update-path': {
                const value = argument === '--update-path';
                if (options.updatePath !== undefined && options.updatePath !== value) {
                    throw new Error('--update-path and --no-update-path cannot be combined');
                }
                options.updatePath = value;
                break;
            }
            case '--reinstall': options.reinstall = true; break;
            case '--allow-downgrade': options.allowDowngrade = true; break;
            case '--overwrite': options.overwrite = true; break;
            case '-h': case '--help': options.help = true; break;
            default: throw new Error(`Unknown install-worker option: ${argument}`);
        }
    }
    if (options.source !== 'github') throw new Error(`Unsupported source: ${options.source}; only github is supported`);
    return options;
}

/** EOF and cancellation resolve as a declined prompt, without waiting indefinitely. */
export function confirmPath(input: Readable, output: Writable, signal?: AbortSignal): Promise<boolean> {
    return new Promise(resolve => {
        const reader = createInterface({ input, output, terminal: false });
        let settled = false;
        const finish = (accepted: boolean) => {
            if (settled) return;
            settled = true;
            signal?.removeEventListener('abort', abort);
            reader.close();
            resolve(accepted);
        };
        const abort = () => finish(false);
        reader.once('close', () => finish(false));
        reader.once('SIGINT', abort);
        reader.once('line', answer => finish(/^(y|yes)$/i.test(answer.trim())));
        signal?.addEventListener('abort', abort, { once: true });
        output.write('Update the managed worker PATH block in ~/.bashrc? [y/N] ');
        if (signal?.aborted || input.readableEnded || input.destroyed) finish(false);
    });
}

/** Explicit dependencies keep orchestration testable using fixture archives and isolated homes. */
export interface InstallDependencies {
    home?: string;
    fetcher?: Fetch;
    checkHost?: () => Promise<void>;
    checkStartup?: (directory: string) => Promise<void>;
    release?: (source: string, version?: string) => Promise<WorkerRelease>;
    input?: Readable;
    output?: Writable;
    interactive?: boolean;
    signal?: AbortSignal;
    /** Narrow filesystem seam for cleanup failure testing; publication/recovery use real filesystem operations. */
    removeObsolete?: (path: string) => Promise<void>;
}

/** PATH failure returns a failure code but never rolls back a successful worker installation. */
export async function installWorker(argv: string[], dependencies: InstallDependencies = {}): Promise<number> {
    const home = dependencies.home ?? homedir();
    const output = dependencies.output ?? process.stdout;
    const print = (message: string) => output.write(`${message}\n`);
    const options = parseInstallArguments(argv, home);
    if (options.help) { output.write(INSTALL_USAGE); return 0; }
    await (dependencies.checkHost ?? validateHost)();
    dependencies.signal?.throwIfAborted();
    const destination = await destinationPath(options.directory, home);
    const release = await (dependencies.release ?? ((source, version) =>
        resolveRelease(source, version, dependencies.fetcher, dependencies.signal)))(options.source, options.version);
    const transaction = new InstallationTransaction(destination,
        message => print(`Warning: ${message}`), dependencies.removeObsolete);
    await transaction.open();
    try {
        const installed = await readInstalled(destination);
        print(`Source: github; installed: ${installed?.version ?? 'unknown/not installed'}; target: ${release.version}`);
        print(`Directory: ${destination}`);
        const action = installationAction(installed, release.version, options);
        const nonempty = await exists(destination) && (await readdir(destination)).length > 0;
        if (!installed && nonempty && !options.overwrite) {
            throw new Error('Unknown non-empty installation directory; replacement requires --overwrite (custom files will be removed)');
        }
        dependencies.signal?.throwIfAborted();
        if (action === 'current') {
            try { await validateTree(destination); }
            catch (error) { throw new Error(`Current installation is incomplete; use --reinstall: ${(error as Error).message}`); }
            print(`Worker ${release.version} is already current; no replacement needed.`);
        } else {
            print('Preparing the complete release; stop workers using this installation before replacement.');
            await mkdir(transaction.preparation, { mode: 0o700 });
            const tree = await prepareArchive(release, transaction.preparation, dependencies.fetcher, dependencies.signal);
            await (dependencies.checkStartup ?? smokeCheck)(tree);
            await atomicWrite(join(tree, METADATA), `${JSON.stringify({ source: 'github', version: release.version,
                installedAt: new Date().toISOString() }, null, 2)}\n`);
            dependencies.signal?.throwIfAborted();
            // Once publication begins, finish it or roll it back before honoring cancellation.
            await transaction.replace(tree);
            print(`Installed worker ${release.version} successfully.`);
        }
    } finally {
        await transaction.close();
    }
    const interactive = dependencies.interactive ?? Boolean(process.stdin.isTTY && process.stdout.isTTY);
    const update = dependencies.signal?.aborted ? false : options.updatePath ?? (interactive && !destination.includes(':')
        ? await confirmPath(dependencies.input ?? process.stdin, output, dependencies.signal) : false);
    let code = dependencies.signal?.aborted ? 130 : 0;
    if (update) {
        try {
            await updateBashrc(home, destination);
            print('PATH block updated in ~/.bashrc. Open a new Bash shell (or source ~/.bashrc), then restart the Hub.');
        } catch (error) {
            print(`Worker is installed, but PATH update failed: ${(error as Error).message}`);
            code = 1;
        }
    } else print('~/.bashrc was not changed. Existing shells and running Hubs keep their current PATH.');
    try {
        print(`Manual PATH: ${pathCommand(destination)}`);
    } catch (error) {
        print(`PATH not available for this directory: ${(error as Error).message}`);
    }
    print(`Verify: ${bashQuote(join(destination, 'bin/simplex'))} run --help`);
    print(`Without PATH changes, use ${join(destination, 'bin/simplex')} as launcher.command[0] in your saved local launch configuration.`);
    return code;
}

/** SIGINT/SIGTERM abort preparation/prompts; publication remains a lifetime fence. */
export async function runInstallCommand(argv: string[]): Promise<number> {
    const controller = new AbortController();
    const interrupt = () => controller.abort(new Error('Worker installation cancelled'));
    process.on('SIGINT', interrupt);
    process.on('SIGTERM', interrupt);
    try { return await installWorker(argv, { signal: controller.signal }); }
    finally {
        process.off('SIGINT', interrupt);
        process.off('SIGTERM', interrupt);
    }
}
