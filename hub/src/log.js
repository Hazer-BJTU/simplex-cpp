/**
 * @file logger for the hub process.
 *
 * The hub is a single process with a single event loop, so logging is
 * intentionally simple: severity filtering, a UTC timestamp, a scope label,
 * and one line per call on a single stream. Worker output is *not* routed
 * through this logger — it is captured per worker and exposed as a bounded log
 * buffer (see src/launch/supervisor.js).
 */

const LEVELS = ['trace', 'debug', 'info', 'warn', 'error', 'silent'];

/** Numeric rank of a level name; unknown names rank as `info`. */
function rank(level) {
    const index = LEVELS.indexOf(level);
    return index === -1 ? LEVELS.indexOf('info') : index;
}

/** Render one log line: `[timestamp] [LEVEL] [scope] message`. */
function format(level, scope, args) {
    const stamp = new Date().toISOString();
    const label = level.toUpperCase().padEnd(5);
    const text = args
        .map((value) => (typeof value === 'string' ? value : inspect(value)))
        .join(' ');
    return `[${stamp}] [${label}] [${scope}] ${text}`;
}

/** Compact JSON rendering for structured log arguments. */
function inspect(value) {
    if (value instanceof Error) {
        return value.stack ?? `${value.name}: ${value.message}`;
    }
    try {
        return JSON.stringify(value);
    } catch {
        return String(value);
    }
}

/**
 * Create a logger.
 *
 * @param {object} [options]
 * @param {string} [options.level] minimum level to emit; default `info`.
 * @param {string} [options.scope] label printed for every line.
 * @param {(line: string) => void} [options.sink] line sink; default stderr.
 * @returns {{trace: Function, debug: Function, info: Function, warn: Function,
 *            error: Function, child: (scope: string) => object, level: string}}
 */
export function createLogger({ level = 'info', scope = 'hub', sink } = {}) {
    const minimum = rank(level);
    const write = sink ?? ((line) => process.stderr.write(`${line}\n`));

    const emit = (name) => (...args) => {
        if (rank(name) < minimum) return;
        write(format(name, scope, args));
    };

    const logger = {
        level,
        trace: emit('trace'),
        debug: emit('debug'),
        info: emit('info'),
        warn: emit('warn'),
        error: emit('error'),
        /** Derive a logger that shares the sink but adds a scope label. */
        child: (childScope) => createLogger({ level, scope: `${scope}:${childScope}`, sink: write }),
    };
    return logger;
}

/** True when `name` is a level this module understands. */
export function isLogLevel(name) {
    return LEVELS.includes(name);
}
