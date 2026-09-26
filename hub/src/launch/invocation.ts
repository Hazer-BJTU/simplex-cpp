/**
 * @file what a launcher is given and what it produces.
 *
 * Both launchers — the bundled worker binary and a deployment's own command
 * template — take the same inputs and answer with the same shape, which is what
 * lets the supervisor spawn either without knowing which one it has. The types
 * live here rather than in one of the builders so neither has to import the
 * other.
 *
 * The configuration and spec interfaces are deliberately narrow: they name only
 * the fields the launchers read, so this module does not depend on the shape of
 * the whole hub configuration, and a launcher cannot reach for something it was
 * never given.
 */

/** Where a worker connects back to, with its session token already applied. */
export interface WorkerEndpoints {
    events: string;
    confirm: string;
}

/** A process to spawn for one session. */
export interface LauncherInvocation {
    command: string;
    args: string[];
    cwd: string;
    env: Record<string, string>;
    /**
     * Set when the launcher daemonizes: the supervisor then signals the pid in
     * this file instead of the pid it spawned, which would name a wrapper.
     */
    pidFile: string | null;
}

/** The slice of hub configuration a launcher reads. */
export interface LauncherConfig {
    dataDir: string;
    worker: { bin: string; args: string[]; promptsDir: string };
    launcher: {
        kind: string;
        command: string[];
        args: string[];
        cwd: string;
        pidFile: string;
    };
}

/** The slice of a normalized session spec a launcher reads. */
export interface LauncherSpec {
    threads: number;
    env: Record<string, string>;
    extraArgs: string[];
}

/** Everything a launcher is handed for one session. */
export interface LauncherInput {
    config: LauncherConfig;
    sessionId: string;
    spec: LauncherSpec;
    /** Path of the generated worker configuration, whether or not it is used. */
    configPath: string;
    sessionDir: string;
    endpoints: WorkerEndpoints;
    token: string;
}
