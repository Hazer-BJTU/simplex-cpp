/**
 * @file default launcher: `simplex_worker --config ... --session ... --threads`.
 *
 * The generated configuration carries the endpoints, provider profile,
 * persistence directory, and prompt path, so the command line stays exactly the
 * one the worker documents in core/README.md.
 */
import type { LauncherInput, LauncherInvocation } from './invocation.ts';

/** Placeholder-free invocation for the bundled worker binary. */
export function buildSimplexWorkerInvocation({
    config, sessionId, spec, configPath, sessionDir,
}: LauncherInput): LauncherInvocation {
    const args = [
        '--config', configPath,
        '--session', sessionId,
        '--threads', String(spec.threads),
    ];
    return {
        command: config.worker.bin,
        // launcher.args are operator additions for this deployment; worker.args
        // are hub-wide worker flags; extraArgs are per-session.
        args: [...args, ...config.launcher.args, ...config.worker.args, ...spec.extraArgs],
        // The worker resolves explicit relative paths against its configuration
        // file and discovers plugins relative to its own executable, so the
        // working directory only matters for tools that inspect it.
        cwd: sessionDir,
        env: { ...spec.env },
        pidFile: config.launcher.pidFile || null,
    };
}
