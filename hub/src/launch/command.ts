/**
 * @file template launcher: run a wrapper command instead of the worker binary.
 *
 * This is the extension point for a deployment that starts workers some other
 * way — a bash wrapper, a container entry point, or a future
 * `simplex run <session>` front end. The hub fills placeholders and does not
 * assume anything else about the command.
 */
import type { LauncherInput, LauncherInvocation } from './invocation.ts';

/** Placeholders a command template may use. */
export const PLACEHOLDERS = [
    'session', 'config', 'data_dir', 'session_dir',
    'endpoint', 'confirm_endpoint', 'token',
    'threads', 'worker_bin', 'prompts_dir',
] as const;

/** One placeholder name. */
export type Placeholder = (typeof PLACEHOLDERS)[number];

/** The values a template is expanded against. */
export type PlaceholderValues = Record<Placeholder, string>;

/**
 * Replace `{name}` placeholders in one template element.
 *
 * @throws {Error} for an unknown placeholder, so a typo fails at spawn time
 *   with the offending name instead of silently passing `{typo}` through.
 */
export function expandTemplate(text: string, values: PlaceholderValues): string {
    return String(text).replace(/\{([A-Za-z_][A-Za-z0-9_]*)\}/g, (match, name: string) => {
        if (!Object.hasOwn(values, name)) {
            throw new Error(`unknown launcher placeholder ${match}; known: ${PLACEHOLDERS.join(', ')}`);
        }
        return values[name as Placeholder];
    });
}

/** Build a process invocation from the configured command template. */
export function buildCommandInvocation({
    config, sessionId, spec, configPath, sessionDir, endpoints, token,
}: LauncherInput): LauncherInvocation {
    const values: PlaceholderValues = {
        session: sessionId,
        config: configPath,
        data_dir: config.dataDir,
        session_dir: sessionDir,
        endpoint: endpoints.events,
        confirm_endpoint: endpoints.confirm,
        token,
        threads: String(spec.threads),
        worker_bin: config.worker.bin,
        prompts_dir: config.worker.promptsDir,
    };
    const template = config.launcher.command.map((part) => expandTemplate(part, values));
    const extra = config.launcher.args.map((part) => expandTemplate(part, values));
    const [command, ...rest] = template;
    return {
        // Configuration validation refuses an empty command template, so the
        // first element exists; the fallback keeps the type honest without a
        // non-null assertion.
        command: command ?? '',
        args: [...rest, ...extra, ...spec.extraArgs],
        cwd: config.launcher.cwd || sessionDir,
        env: { ...spec.env },
        pidFile: config.launcher.pidFile || null,
    };
}
