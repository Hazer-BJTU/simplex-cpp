/** Host probes use executables/shared-library reports, not Node's bundled OpenSSL. */
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { join } from 'node:path';

const exec = promisify(execFile);

/** Do not inherit a build environment's dynamic-library injection/search paths. */
export function runtimeEnvironment(): NodeJS.ProcessEnv {
    const env = { ...process.env };
    delete env.LD_LIBRARY_PATH;
    delete env.LD_PRELOAD;
    delete env.LD_AUDIT;
    return env;
}

export function validatePlatform(platform: string, arch: string, glibc: string | undefined): void {
    if (platform !== 'linux' || arch !== 'x64') {
        throw new Error(`Worker releases require Linux x86_64; this host is ${platform}/${arch}`);
    }
    const match = /^(\d+)\.(\d+)/.exec(glibc ?? '');
    if (!match || Number(match[1]) < 2 || (Number(match[1]) === 2 && Number(match[2]) < 34)) {
        throw new Error(`Worker releases require glibc >= 2.34; detected ${glibc ?? 'no glibc (possibly musl)'}`);
    }
}

export async function validateHost(): Promise<void> {
    const header = (process.report.getReport() as { header: { glibcVersionRuntime?: string } }).header;
    validatePlatform(process.platform, process.arch, header.glibcVersionRuntime);
    for (const [command, args, requirement] of [
        ['bash', ['--version'], 'Bash'],
        ['flock', ['--version'], 'util-linux flock'],
        ['ldd', ['--version'], 'the glibc ldd utility'],
    ] as const) {
        try {
            await exec(command, [...args], { timeout: 5000, maxBuffer: 64 * 1024, env: runtimeEnvironment() });
        } catch {
            throw new Error(`Install ${requirement} on the Hub host before installing the worker`);
        }
    }
}

/** The host loader proves libssl.so.3/libcrypto.so.3 can resolve for this worker. */
export async function smokeCheck(directory: string): Promise<void> {
    const options = { timeout: 15_000, maxBuffer: 256 * 1024, env: runtimeEnvironment() };
    try {
        const { stdout, stderr } = await exec('ldd', [join(directory, 'bin/simplex_worker')], options);
        const libraries = stdout + stderr;
        if (/not found/.test(libraries) || !/libssl\.so\.3\s+=>\s+\//.test(libraries)
            || !/libcrypto\.so\.3\s+=>\s+\//.test(libraries)) {
            throw new Error('Host must provide OpenSSL 3 (libssl.so.3 and libcrypto.so.3) and all worker runtime libraries');
        }
        await exec(join(directory, 'bin/simplex'), ['run', '--help'], options);
    } catch (error) {
        throw new Error(`Worker startup check failed; check host OpenSSL 3/runtime libraries: ${(error as Error).message}`);
    }
}
