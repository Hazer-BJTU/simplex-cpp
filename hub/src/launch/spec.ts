/**
 * @file per-session launch specification.
 *
 * A session spec is the deployment-side description of how one worker should
 * run: which provider profile, how many threads, where its prompt lives, what
 * it should say about its environment, and how it persists state. Defaults come
 * from the hub configuration; a session overrides only what it needs, so a
 * stored session stays readable when the hub's defaults change. Configuration
 * fields seed the first config/config.yaml only; subsequent starts reuse that
 * file. Threads, environment variables and extra command arguments remain
 * launch-time settings.
 */
import { ConfigError, checkPromptFile } from '../config.ts';
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
    /** Profile for a separate assistant role, or null to omit the role. */
    modalityAssistProvider: string | null;
    threads: number;
    maxExchanges: number;
    autoCompactThreshold: number;
    maxAutoCompactions: number;
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
    // DeepSeek sessions use the configured DeepSeek profile by default. Other
    // providers only acquire an additional model dependency by explicit choice.
    const defaultAssist = provider === 'deepseek' && provider !== config.mock.profile
        ? 'deepseek' : null;
    const assist = raw.modalityAssistProvider === undefined
        ? defaultAssist : raw.modalityAssistProvider;
    if (assist !== null && (typeof assist !== 'string'
        || !Object.hasOwn(config.providerProfiles, assist)
        || assist === config.mock.profile)) {
        throw new ConfigError('spec.modalityAssistProvider must name a non-mock provider profile or be null');
    }
    const positive = (value: unknown, fallback: number, name: string): number => {
        const resolved = value ?? fallback;
        if (!Number.isInteger(resolved) || (resolved as number) <= 0) {
            throw new ConfigError(`spec.${name} must be a positive integer`);
        }
        return resolved as number;
    };
    const autoCompactThreshold = raw.autoCompactThreshold ?? config.worker.autoCompactThreshold;
    const maxAutoCompactions = raw.maxAutoCompactions ?? config.worker.maxAutoCompactions;
    if (!Number.isSafeInteger(autoCompactThreshold) || (autoCompactThreshold as number) < 0
        || (autoCompactThreshold as number) > 2147483647
        || !Number.isSafeInteger(maxAutoCompactions) || (maxAutoCompactions as number) <= 0
        || (maxAutoCompactions as number) > 2147483647) {
        throw new ConfigError('invalid auto compact threshold or attempt budget');
    }
    const restore = raw.restore ?? 'if_present';
    if (!(RESTORE_POLICIES as readonly unknown[]).includes(restore)) {
        throw new ConfigError(`spec.restore must be one of ${RESTORE_POLICIES.join(', ')}`);
    }
    const software = raw.software ?? config.worker.environment.software ?? [];
    if (!Array.isArray(software) || software.some((item) => typeof item !== 'string')) {
        throw new ConfigError('spec.software must be an array of strings');
    }
    const promptFile = checkPromptFile(
        raw.systemPromptFile ?? config.worker.systemPromptFile, 'spec.systemPromptFile');
    const persistence = isPlainObject(raw.persistence) ? raw.persistence : {};
    return {
        provider,
        // Empty means "use the profile's model".
        model: typeof raw.model === 'string' ? raw.model : '',
        modalityAssistProvider: assist,
        threads: positive(raw.threads, config.worker.threads, 'threads'),
        autoCompactThreshold: autoCompactThreshold as number,
        maxAutoCompactions: maxAutoCompactions as number,
        maxExchanges: positive(raw.maxExchanges, config.worker.maxExchanges, 'maxExchanges'),
        eventCapacity: positive(raw.eventCapacity, config.worker.eventCapacity, 'eventCapacity'),
        // Relative to the worker's installation directory, and stored exactly
        // as given: the hub never rewrites it into a path of its own, so one
        // session spec describes the same prompt on every machine.
        systemPromptFile: promptFile,
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
