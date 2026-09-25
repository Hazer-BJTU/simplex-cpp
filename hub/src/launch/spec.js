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
import { ConfigError } from '../config.js';

/** Persistence restore policies accepted by the worker. */
export const RESTORE_POLICIES = ['if_present', 'never'];

/** Keep only the fields a session may store, with defaults applied. */
export function normalizeSpec(config, spec = {}) {
    const profiles = Object.keys(config.providerProfiles);
    const provider = spec.provider ?? profiles[0];
    if (!Object.hasOwn(config.providerProfiles, provider)) {
        throw new ConfigError(
            `unknown provider profile "${provider}"; configured profiles: ${profiles.join(', ')}`);
    }
    const positive = (value, fallback, name) => {
        const resolved = value ?? fallback;
        if (!Number.isInteger(resolved) || resolved <= 0) {
            throw new ConfigError(`spec.${name} must be a positive integer`);
        }
        return resolved;
    };
    const restore = spec.restore ?? 'if_present';
    if (!RESTORE_POLICIES.includes(restore)) {
        throw new ConfigError(`spec.restore must be one of ${RESTORE_POLICIES.join(', ')}`);
    }
    const software = spec.software ?? config.worker.environment.software ?? [];
    if (!Array.isArray(software) || software.some((item) => typeof item !== 'string')) {
        throw new ConfigError('spec.software must be an array of strings');
    }
    const promptFile = spec.systemPromptFile ?? config.worker.systemPromptFile;
    if (typeof promptFile !== 'string' || promptFile.length === 0) {
        throw new ConfigError('spec.systemPromptFile must be a nonempty path');
    }
    return {
        provider,
        // Empty means "use the profile's model".
        model: typeof spec.model === 'string' ? spec.model : '',
        threads: positive(spec.threads, config.worker.threads, 'threads'),
        maxExchanges: positive(spec.maxExchanges, config.worker.maxExchanges, 'maxExchanges'),
        eventCapacity: positive(spec.eventCapacity, config.worker.eventCapacity, 'eventCapacity'),
        // A bare file name selects a file in the configured prompts directory;
        // an absolute path is used as given.
        systemPromptFile: isAbsolute(promptFile)
            ? promptFile
            : join(config.worker.promptsDir, promptFile),
        workspace: typeof spec.workspace === 'string' ? spec.workspace : config.worker.environment.workspace,
        platform: typeof spec.platform === 'string' ? spec.platform : config.worker.environment.platform,
        software: [...software],
        persistence: {
            enabled: spec.persistence?.enabled ?? config.worker.persistence.enabled,
            readable: spec.persistence?.readable ?? config.worker.persistence.readable,
        },
        restore,
        env: spec.env && typeof spec.env === 'object' ? { ...spec.env } : {},
        extraArgs: Array.isArray(spec.extraArgs) ? [...spec.extraArgs] : [],
    };
}
