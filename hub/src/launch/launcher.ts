/**
 * @file launcher selection.
 *
 * A launcher turns a session into a process invocation. It is a separate
 * concept from configuration rendering on purpose: the deployment may replace
 * `simplex_worker --config ... --session ...` with a wrapper script or a future
 * `simplex run <session>` front end without changing how the hub talks to
 * workers, how it renders configuration, or how the panel behaves.
 *
 * Two kinds ship today:
 *   - `simplex-worker`: run `worker.bin` with the generated configuration.
 *   - `command`: run a template, with `{session}`, `{config}`, `{data_dir}`,
 *     `{session_dir}`, `{endpoint}`, `{confirm_endpoint}`, `{token}`,
 *     `{threads}`, `{worker_bin}`, and `{prompts_dir}` placeholders.
 *
 * A launcher may own its configuration (`launcher.config: "launcher"`), in
 * which case the hub writes no configuration file and the template's `{config}`
 * expands to the path it *would* have written. A launcher that daemonizes must
 * declare `launcher.pidFile`; the supervisor then signals that pid instead of
 * the process it spawned. See hub/docs/worker-adapter.md.
 */
import { LAUNCHER_KINDS } from '../config.ts';
import type { HubConfig } from '../config.ts';
import type { Logger } from '../log.ts';
import { buildCommandInvocation } from './command.ts';
import { buildSimplexWorkerInvocation } from './simplex-worker.ts';
import type { LauncherInput, LauncherInvocation } from './invocation.ts';

export { LAUNCHER_KINDS };

/** What a launcher is asked for, minus the configuration it already holds. */
export type InvocationContext = Omit<LauncherInput, 'config'>;

/** The launcher facade the supervisor drives. */
export interface Launcher {
    kind: string;
    /** True when the launcher renders its own worker configuration. */
    ownsConfig: boolean;
    /** True when the launcher may outlive the process the hub spawned. */
    mayDaemonize: boolean;
    buildInvocation: (context: InvocationContext) => LauncherInvocation;
}

/** Everything `createLauncher` needs. */
export interface LauncherOptions {
    config: HubConfig;
    /** Accepted for call-site uniformity; the launcher itself does not log. */
    log?: Logger;
}

/** Create the configured launcher. */
export function createLauncher({ config }: LauncherOptions): Launcher {
    const kind = config.launcher.kind;
    if (!(LAUNCHER_KINDS as readonly string[]).includes(kind)) {
        throw new Error(`unsupported launcher kind "${kind}"`);
    }
    const build = kind === 'command' ? buildCommandInvocation : buildSimplexWorkerInvocation;
    return {
        kind,
        /** True when the launcher renders its own worker configuration. */
        ownsConfig: config.launcher.config === 'launcher',
        /** True when the launcher may outlive the process the hub spawned. */
        mayDaemonize: Boolean(config.launcher.pidFile) || kind === 'command',
        buildInvocation: (context) => build({ config, ...context }),
    };
}
