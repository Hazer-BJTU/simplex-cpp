/** User-facing package choices are separate from their release asset suffixes. */
export const WORKER_PLATFORMS = {
    'linux-amd64-standard': {
        suffix: 'linux-x86_64-glibc2.34',
        description: 'Linux x86_64; requires host glibc >= 2.34 and OpenSSL 3. C++ runtimes are bundled.',
    },
    'linux-amd64-bundled': {
        suffix: 'linux-x86_64-musl',
        description: 'Linux x86_64; bundles the musl loader, OpenSSL and runtime dependencies. No host glibc/OpenSSL required.',
    },
} as const;

export type WorkerPlatform = keyof typeof WORKER_PLATFORMS;

/** Fail before host probes, downloads or filesystem changes for unknown tags. */
export function parseWorkerPlatform(value: string): WorkerPlatform {
    if (!Object.hasOwn(WORKER_PLATFORMS, value)) {
        throw new Error(`Unsupported platform: ${value}; supported: ${Object.keys(WORKER_PLATFORMS).join(', ')}`);
    }
    return value as WorkerPlatform;
}
