#!/usr/bin/env node
/**
 * @file `simplex-hub` command-line entry point.
 *
 * Parses operator options, loads the hub configuration, starts the hub, and
 * shuts it down cleanly on SIGINT/SIGTERM (worker children are released by the
 * supervisor, not by an abrupt exit).
 */
import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { ConfigError, hubRoot, loadConfig } from '../src/config.js';
import { createHub } from '../src/hub.js';
import { createLogger, isLogLevel } from '../src/log.js';

const USAGE = `Usage: simplex-hub [options]

Options:
  -c, --config <file>            hub configuration file (default: hub.config.jsonc)
  -l, --listen <host:port>       listen address (default: 127.0.0.1:8800)
      --data-dir <dir>           runtime data directory (default: ./data)
      --worker-bin <path>        simplex_worker executable
      --prompts-dir <dir>        directory holding worker prompt YAML files
      --panel-token <token>      require this token for panel access
      --mock                     enable the offline mock provider
      --no-mock                  disable the offline mock provider
      --force-kill-process-group kill the worker's process group on force stop
  -v, --verbose                  log at debug level
      --log-level <level>        trace|debug|info|warn|error|silent
  -h, --help                     show this message
  -V, --version                  show the hub version

The worker-facing endpoints are documented in core/docs/worker-protocol.md;
the panel API is documented in hub/docs/hub-protocol.md.
`;

/** Read the hub package version without importing JSON modules. */
function packageVersion() {
    try {
        const here = dirname(fileURLToPath(import.meta.url));
        const text = readFileSync(resolve(here, '..', 'package.json'), 'utf8');
        return JSON.parse(text).version ?? '0.0.0';
    } catch {
        return '0.0.0';
    }
}

/** Parse `host:port`, accepting bracketed IPv6 hosts. */
export function parseListen(text) {
    const trimmed = text.trim();
    const match = /^(?:\[(?<v6>[^\]]+)\]|(?<host>[^:]*)):(?<port>\d+)$/.exec(trimmed);
    if (!match) throw new ConfigError(`--listen expects host:port, got "${text}"`);
    const port = Number.parseInt(match.groups.port, 10);
    if (!Number.isInteger(port) || port < 0 || port > 65535) {
        throw new ConfigError(`--listen port out of range: ${match.groups.port}`);
    }
    const host = match.groups.v6 ?? match.groups.host;
    if (!host) throw new ConfigError(`--listen needs an explicit host, got "${text}"`);
    return { host, port };
}

/** Parse process arguments into configuration overrides plus run options. */
export function parseArguments(argv) {
    const overrides = {};
    const run = { help: false, version: false, verbose: false, logLevel: null, mock: null };
    const need = (index, flag) => {
        if (index + 1 >= argv.length) throw new ConfigError(`${flag} requires a value`);
        return argv[index + 1];
    };
    for (let index = 0; index < argv.length; index += 1) {
        const flag = argv[index];
        switch (flag) {
            case '-h': case '--help': run.help = true; break;
            case '-V': case '--version': run.version = true; break;
            case '-v': case '--verbose': run.verbose = true; break;
            case '-c': case '--config':
                run.config = need(index, flag); index += 1; break;
            case '-l': case '--listen':
                overrides.listen = parseListen(need(index, flag)); index += 1; break;
            case '--data-dir':
                overrides.dataDir = need(index, flag); index += 1; break;
            case '--worker-bin':
                overrides.worker = { ...overrides.worker, bin: need(index, flag) }; index += 1; break;
            case '--prompts-dir':
                overrides.worker = { ...overrides.worker, promptsDir: need(index, flag) }; index += 1; break;
            case '--panel-token':
                overrides.panel = { token: need(index, flag) }; index += 1; break;
            case '--mock': run.mock = true; break;
            case '--no-mock': run.mock = false; break;
            case '--force-kill-process-group': overrides.forceKillProcessGroup = true; break;
            case '--log-level': {
                const level = need(index, flag);
                if (!isLogLevel(level)) throw new ConfigError(`unknown log level "${level}"`);
                run.logLevel = level; index += 1; break;
            }
            default:
                throw new ConfigError(`unknown option "${flag}"`);
        }
    }
    if (run.mock !== null) overrides.mock = { enabled: run.mock };
    return { overrides, run };
}

/** Start the hub and keep it running until a signal arrives. */
export async function main(argv = process.argv.slice(2)) {
    const { overrides, run } = parseArguments(argv);
    if (run.help) {
        process.stdout.write(USAGE);
        return 0;
    }
    if (run.version) {
        process.stdout.write(`${packageVersion()}\n`);
        return 0;
    }
    const level = run.logLevel ?? (run.verbose ? 'debug' : 'info');
    const log = createLogger({ level });
    const { config, file } = loadConfig({ file: run.config, overrides });
    log.info(file ? `configuration: ${file}` : 'configuration: built-in defaults');
    log.info(`data directory: ${config.dataDir}`);

    const hub = createHub({ config, log, hubRoot, version: packageVersion() });
    const address = await hub.start();
    log.info(`panel: ${address.url}`);

    let stopping = false;
    const stop = async (signal) => {
        if (stopping) return;
        stopping = true;
        log.info(`received ${signal}; shutting down`);
        await hub.stop();
    };
    process.on('SIGINT', () => { void stop('SIGINT'); });
    process.on('SIGTERM', () => { void stop('SIGTERM'); });
    return { hub, address, stop };
}

// Only run when executed directly, so tests can import main/parseArguments.
if (process.argv[1] && fileURLToPath(import.meta.url) === resolve(process.argv[1])) {
    main().then((result) => {
        if (typeof result === 'number') process.exitCode = result;
    }).catch((error) => {
        process.stderr.write(`simplex-hub: ${error.message}\n`);
        if (!(error instanceof ConfigError)) process.stderr.write(`${error.stack}\n`);
        process.exitCode = 1;
    });
}
