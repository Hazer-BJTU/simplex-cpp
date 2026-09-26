/**
 * @file logger for the hub process.
 *
 * The hub is a single process with a single event loop, so logging is
 * intentionally simple: severity filtering, a UTC timestamp, a scope label,
 * and one line per call on a single stream. Worker output is *not* routed
 * through this logger — it is captured per worker and exposed as a bounded log
 * buffer (see src/launch/supervisor.js).
 */

const LEVELS = ['trace', 'debug', 'info', 'warn', 'error', 'silent'] as const;

/** A level this module understands. */
export type LogLevel = (typeof LEVELS)[number];

/** The logger handed to every module that logs. */
export interface Logger {
    /** The level this logger was created with. */
    readonly level: string;
    trace: (...args: unknown[]) => void;
    debug: (...args: unknown[]) => void;
    info: (...args: unknown[]) => void;
    warn: (...args: unknown[]) => void;
    error: (...args: unknown[]) => void;
    /** Derive a logger that shares the sink but adds a scope label. */
    child: (scope: string) => Logger;
}

/** Options accepted by `createLogger`. */
export interface LoggerOptions {
    /** Minimum level to emit; default `info`. An unknown name ranks as `info`. */
    level?: string;
    /** Label printed for every line; default `hub`. */
    scope?: string;
    /** Line sink; default stderr. */
    sink?: (line: string) => void;
}

/** Numeric rank of a level name; unknown names rank as `info`. */
function rank(level: string): number {
    const index = (LEVELS as readonly string[]).indexOf(level);
    return index === -1 ? LEVELS.indexOf('info') : index;
}

/** Render one log line: `[timestamp] [LEVEL] [scope] message`. */
function format(level: string, scope: string, args: unknown[]): string {
    const stamp = new Date().toISOString();
    const label = level.toUpperCase().padEnd(5);
    const text = args
        .map((value) => (typeof value === 'string' ? value : inspect(value)))
        .join(' ');
    return `[${stamp}] [${label}] [${scope}] ${text}`;
}

/** Compact JSON rendering for structured log arguments. */
function inspect(value: unknown): string {
    if (value instanceof Error) {
        return value.stack ?? `${value.name}: ${value.message}`;
    }
    try {
        // `JSON.stringify` returns undefined for a bare function or symbol,
        // which `String(value)` covers.
        return JSON.stringify(value) ?? String(value);
    } catch {
        return String(value);
    }
}

/** Create a logger. */
export function createLogger({ level = 'info', scope = 'hub', sink }: LoggerOptions = {}): Logger {
    const minimum = rank(level);
    const write = sink ?? ((line: string) => process.stderr.write(`${line}\n`));

    const emit = (name: string) => (...args: unknown[]) => {
        if (rank(name) < minimum) return;
        write(format(name, scope, args));
    };

    const logger: Logger = {
        level,
        trace: emit('trace'),
        debug: emit('debug'),
        info: emit('info'),
        warn: emit('warn'),
        error: emit('error'),
        child: (childScope) => createLogger({ level, scope: `${scope}:${childScope}`, sink: write }),
    };
    return logger;
}

/** True when `name` is a level this module understands. */
export function isLogLevel(name: unknown): name is LogLevel {
    return typeof name === 'string' && (LEVELS as readonly string[]).includes(name);
}
