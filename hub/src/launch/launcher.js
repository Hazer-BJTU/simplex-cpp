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
import { buildCommandInvocation } from './command.ts';
import { buildSimplexWorkerInvocation } from './simplex-worker.ts';

export { LAUNCHER_KINDS };

/**
 * Create the configured launcher.
 *
 * @param {object} options
 * @param {object} options.config hub configuration (already validated).
 * @param {object} options.log hub logger.
 * @returns {{kind: string, ownsConfig: boolean, buildInvocation: Function}}
 */
export function createLauncher({ config, log }) {
    const kind = config.launcher.kind;
    if (!LAUNCHER_KINDS.includes(kind)) {
        throw new Error(`unsupported launcher kind "${kind}"`);
    }
    const build = kind === 'command' ? buildCommandInvocation : buildSimplexWorkerInvocation;
    return {
        kind,
        /** True when the launcher renders its own worker configuration. */
        ownsConfig: config.launcher.config === 'launcher',
        /** True when the launcher may outlive the process the hub spawned. */
        mayDaemonize: Boolean(config.launcher.pidFile) || kind === 'command',
        /**
         * @param {object} context
         * @param {string} context.sessionId
         * @param {object} context.spec normalized session spec.
         * @param {string} context.configPath configuration file the hub wrote.
         * @param {string} context.sessionDir per-session hub directory.
         * @param {{events: string, confirm: string}} context.endpoints
         * @param {string} context.token session access token.
         * @returns {{command: string, args: string[], cwd: string,
         *            env: object, pidFile: string|null}}
         */
        buildInvocation: (context) => build({ config, log, ...context }),
    };
}
