/**
 * @file per-session launch specification.
 *
 * A session spec is the deployment-side description of how one worker should
 * run: which provider profile, how many threads, where its prompt lives, what
 * it should say about its environment, and how it persists state. Defaults come
 * from the hub configuration; a session overrides only what it needs, so a
 * stored session stays readable when the hub's defaults change.
 */
import { isAbsolute, join } from 'node:path';
import { ConfigError } from '../config.ts';
import type { HubConfig } from '../config.ts';

/** Persistence restore policies accepted by the worker. */
export const RESTORE_POLICIES = ['if_present', 'never'] as const;

/** One restore policy. */
export type RestorePolicy = (typeof RESTORE_POLICIES)[number];

/**
 * A session spec with every field resolved.
 *
 * `normalizeSpec` is the only producer, which is what lets the launchers and
 * the configuration renderer read it without a default at every use.
 */
export interface NormalizedSpec {
    provider: string;
    /** Empty means "use the profile's model". */
    model: string;
    threads: number;
    maxExchanges: number;
    eventCapacity: number;
    systemPromptFile: string;
    workspace: string;
    platform: string;
    software: string[];
    persistence: { enabled: boolean; readable: boolean };
    restore: RestorePolicy;
    env: Record<string, string>;
    extraArgs: string[];
}

/** True for a plain JSON object (not an array, not null). */
function isPlainObject(value: unknown): value is Record<string, unknown> {
    return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/**
 * Keep only the fields a session may store, with defaults applied.
 *
 * The input is `unknown` because it arrives as JSON from the panel or from
 * `hub.json`, and a spec that is not an object is treated as an empty one —
 * which is what this function has always done, and is why the panel's own
 * check refuses a non-object spec before it gets this far.
 */
export function normalizeSpec(config: HubConfig, spec: unknown = {}): NormalizedSpec {
    const raw: Record<string, unknown> = isPlainObject(spec) ? spec : {};
    const profiles = Object.keys(config.providerProfiles);
    const provider = typeof raw.provider === 'string' ? raw.provider : (profiles[0] as string);
    if (!Object.hasOwn(config.providerProfiles, provider)) {
        throw new ConfigError(
            `unknown provider profile "${provider}"; configured profiles: ${profiles.join(', ')}`);
    }
    const positive = (value: unknown, fallback: number, name: string): number => {
        const resolved = value ?? fallback;
        if (!Number.isInteger(resolved) || (resolved as number) <= 0) {
            throw new ConfigError(`spec.${name} must be a positive integer`);
        }
        return resolved as number;
    };
    const restore = raw.restore ?? 'if_present';
    if (!(RESTORE_POLICIES as readonly unknown[]).includes(restore)) {
        throw new ConfigError(`spec.restore must be one of ${RESTORE_POLICIES.join(', ')}`);
    }
    const software = raw.software ?? config.worker.environment.software ?? [];
    if (!Array.isArray(software) || software.some((item) => typeof item !== 'string')) {
        throw new ConfigError('spec.software must be an array of strings');
    }
    const promptFile = raw.systemPromptFile ?? config.worker.systemPromptFile;
    if (typeof promptFile !== 'string' || promptFile.length === 0) {
        throw new ConfigError('spec.systemPromptFile must be a nonempty path');
    }
    const persistence = isPlainObject(raw.persistence) ? raw.persistence : {};
    return {
        provider,
        // Empty means "use the profile's model".
        model: typeof raw.model === 'string' ? raw.model : '',
        threads: positive(raw.threads, config.worker.threads, 'threads'),
        maxExchanges: positive(raw.maxExchanges, config.worker.maxExchanges, 'maxExchanges'),
        eventCapacity: positive(raw.eventCapacity, config.worker.eventCapacity, 'eventCapacity'),
        // A bare file name selects a file in the configured prompts directory;
        // an absolute path is used as given.
        systemPromptFile: isAbsolute(promptFile)
            ? promptFile
            : join(config.worker.promptsDir, promptFile),
        workspace: typeof raw.workspace === 'string'
            ? raw.workspace
            : config.worker.environment.workspace,
        platform: typeof raw.platform === 'string'
            ? raw.platform
            : config.worker.environment.platform,
        software: [...software] as string[],
        persistence: {
            enabled: typeof persistence.enabled === 'boolean'
                ? persistence.enabled
                : config.worker.persistence.enabled,
            readable: typeof persistence.readable === 'boolean'
                ? persistence.readable
                : config.worker.persistence.readable,
        },
        restore: restore as RestorePolicy,
        env: isPlainObject(raw.env) ? { ...raw.env } as Record<string, string> : {},
        extraArgs: Array.isArray(raw.extraArgs) ? [...raw.extraArgs] as string[] : [],
    };
}
