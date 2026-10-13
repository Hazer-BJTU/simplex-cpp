/** Host probes use executables/shared-library reports, not Node's bundled OpenSSL. */
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { join } from 'node:path';
import type { WorkerPlatform } from './platform.ts';

const exec = promisify(execFile);

/** Do not inherit a build environment's dynamic-library injection/search paths. */
export function runtimeEnvironment(): NodeJS.ProcessEnv {
    const env = { ...process.env };
    delete env.LD_LIBRARY_PATH;
    delete env.LD_PRELOAD;
    delete env.LD_AUDIT;
    return env;
}

export function validatePlatform(target: WorkerPlatform, platform: string, arch: string, glibc: string | undefined): void {
    if (platform !== 'linux' || arch !== 'x64') {
        throw new Error(`Worker releases require Linux x86_64; this host is ${platform}/${arch}`);
    }
    if (target === 'linux-amd64-bundled') return;
    const match = /^(\d+)\.(\d+)/.exec(glibc ?? '');
    if (!match || Number(match[1]) < 2 || (Number(match[1]) === 2 && Number(match[2]) < 34)) {
        throw new Error(`Worker releases require glibc >= 2.34; detected ${glibc ?? 'no glibc (possibly musl)'}`);
    }
}

export async function validateHost(target: WorkerPlatform): Promise<void> {
    const header = (process.report.getReport() as { header: { glibcVersionRuntime?: string } }).header;
    validatePlatform(target, process.platform, process.arch, header.glibcVersionRuntime);
    const commands: [string, string[], string][] = [
        ['bash', ['--version'], 'Bash'],
        ['flock', ['--version'], 'util-linux flock'],
    ];
    if (target === 'linux-amd64-standard') {
        commands.push(['ldd', ['--version'], 'the glibc ldd utility']);
    } else {
        commands.push(['dirname', ['/tmp/worker'], 'dirname'], ['readlink', ['--help'], 'readlink']);
    }
    for (const [command, args, requirement] of commands) {
        try {
            await exec(command, [...args], { timeout: 5000, maxBuffer: 64 * 1024, env: runtimeEnvironment() });
        } catch {
            throw new Error(`Install ${requirement} on the Hub host before installing the worker`);
        }
    }
}

/** Validate the selected package with its own launch path and dependency policy. */
export async function smokeCheck(directory: string, target: WorkerPlatform): Promise<void> {
    const options = { timeout: 15_000, maxBuffer: 256 * 1024, env: runtimeEnvironment() };
    try {
        if (target === 'linux-amd64-standard') {
            const { stdout, stderr } = await exec('ldd', [join(directory, 'bin/simplex_worker')], options);
            const libraries = stdout + stderr;
            if (/not found/.test(libraries) || !/libssl\.so\.3\s+=>\s+\//.test(libraries)
                || !/libcrypto\.so\.3\s+=>\s+\//.test(libraries)) {
                throw new Error('Host must provide OpenSSL 3 (libssl.so.3 and libcrypto.so.3) and all worker runtime libraries');
            }
        } else {
            // simplex_worker is a Bash wrapper in this package. Host ldd cannot
            // validate it; use the shipped loader on the real ELF executable.
            const lib = join(directory, 'lib');
            const { stdout } = await exec(join(lib, 'ld-musl-x86_64.so.1'),
                ['--library-path', lib, '--list', join(directory, 'bin/simplex_worker.bin')], options);
            // musl reports its own libc under the ELF interpreter's logical
            // /lib name, even when this bundled loader was launched explicitly.
            // Recognize only that alias at the loader's own mapping address.
            const ownLoader = /^\s*(\/[^\r\n]+?\/ld-musl-x86_64\.so\.1)\s+\((0x[a-f0-9]+)\)\s*$/mi.exec(stdout);
            const dependencies = [...stdout.matchAll(/^\s*(\S+)\s+=>\s+(.+?)\s+\((0x[a-f0-9]+)\)/gmi)];
            for (const [, name, resolved, address] of dependencies) {
                const self = name === 'libc.musl-x86_64.so.1' && ownLoader !== null
                    && resolved === ownLoader[1] && address === ownLoader[2];
                if (!self && !resolved!.startsWith(lib + '/')) {
                    throw new Error(`Bundled dependency resolved outside the package: ${name}`);
                }
            }
            for (const library of ['libssl.so.3', 'libcrypto.so.3']) {
                if (!dependencies.some(([, name, resolved]) => name === library && resolved === join(lib, library))) {
                    throw new Error(`Bundled OpenSSL library did not resolve: ${library}`);
                }
            }
        }
        // Exercise the public router and, for bundled packages, the wrapper's
        // resource lookup. Keep the whole installed directory as published.
        await exec(join(directory, 'bin/simplex'), ['run', '--help'], options);
    } catch (error) {
        const requirements = target === 'linux-amd64-standard'
            ? 'host OpenSSL 3/runtime libraries' : 'bundled loader/runtime libraries';
        throw new Error(`Worker startup check failed for ${target}; check ${requirements}: ${(error as Error).message}`);
    }
}
