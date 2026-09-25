/**
 * @file hub assembly: wires the HTTP front door and every role adapter.
 *
 * One HTTP server carries all three audiences: the browser panel, the JSON API,
 * and the worker-facing WebSocket routes. Each role registers itself here, so
 * this file is the map of what the hub currently implements.
 */
import { createHttpServer, sendJson } from './http/server.js';
import { createLauncher } from './launch/launcher.js';
import { WorkerSupervisor } from './launch/supervisor.js';
import { SessionRegistry } from './state/registry.js';
import { createWorkerConfirmationRoute } from './worker/confirmation.js';
import { createWorkerEventRoute } from './worker/connection.js';

/** Protocol name/version announced by `/api/meta`. */
export const PANEL_PROTOCOL = { name: 'simplex-hub-panel', version: 1 };

/** Capabilities reported by `/api/meta`; extended as roles are implemented. */
export const CAPABILITIES = ['worker-events', 'confirmations', 'supervisor'];

/**
 * Host a worker should connect back to.
 *
 * A wildcard listener must not be echoed into the worker's configuration: the
 * worker needs a routable address, and loopback is the only one the hub can
 * assume for a service that listens on every interface.
 */
export function connectHostFor(host) {
    if (host === '0.0.0.0' || host === '::' || host === '') return '127.0.0.1';
    return host;
}

/**
 * Build a hub instance. Nothing listens until `start()` is called.
 *
 * @param {object} options
 * @param {object} options.config validated hub configuration.
 * @param {object} options.log logger created by src/log.js.
 * @param {string} options.hubRoot absolute `hub/` directory.
 * @param {string} [options.version] hub package version.
 * @param {object} [options.hooks] optional observers used by the panel API.
 * @param {(envelope: object, connection: object) => void} [options.hooks.onEvent]
 * @param {(prompt: object) => void} [options.hooks.onPrompt]
 * @param {(prompt: object, outcome: object) => void} [options.hooks.onPromptSettled]
 * @param {(session: object, connection: object|null) => void} [options.hooks.onConnectionChange]
 * @param {(session: object, record: object|null) => void} [options.hooks.onProcessChange]
 * @param {() => ({baseUrl: string}|null)} [options.hooks.mockProvider]
 */
export function createHub({ config, log, hubRoot, version = '0.0.0', hooks = {} }) {
    const http = createHttpServer({ config, log, hubRoot });
    const registry = new SessionRegistry({ config, log });
    /** Bound address, known only after `start()`. */
    let bound = null;

    /** Worker-facing URLs for one session, valid after the listener is bound. */
    function endpointsFor(sessionId, token) {
        if (!bound) throw new Error('the hub is not listening yet');
        const host = connectHostFor(bound.host);
        const authority = `${host.includes(':') ? `[${host}]` : host}:${bound.port}`;
        const query = `token=${encodeURIComponent(token)}`;
        return {
            events: `ws://${authority}/agent/${sessionId}/events?${query}`,
            confirm: `ws://${authority}/agent/${sessionId}/confirm?${query}`,
        };
    }

    http.route('GET', '/api/meta', ({ res }) => {
        sendJson(res, 200, {
            name: 'simplex-hub',
            version,
            protocol: PANEL_PROTOCOL,
            worker_protocol: 'core/docs/worker-protocol.md',
            capabilities: CAPABILITIES,
            listen: { host: config.listen.host, port: config.listen.port },
            launcher: { kind: config.launcher.kind, owns_config: config.launcher.config === 'launcher' },
            provider_profiles: Object.keys(config.providerProfiles),
            force_kill_process_group: config.forceKillProcessGroup,
        });
    });

    const workerEvents = createWorkerEventRoute({
        registry,
        config,
        log,
        onEvent: (envelope, connection) => {
            connection.log.debug(`event ${envelope.event} (seq ${envelope.sequence})`);
            hooks.onEvent?.(envelope, connection);
        },
        onConnectionChange: hooks.onConnectionChange,
    });
    http.useUpgrade(workerEvents);

    const confirmations = createWorkerConfirmationRoute({
        registry,
        config,
        log,
        onPrompt: hooks.onPrompt,
        onSettled: hooks.onPromptSettled,
    });
    http.useUpgrade(confirmations);

    const supervisor = new WorkerSupervisor({
        config,
        log,
        registry,
        launcher: createLauncher({ config, log }),
        endpointsFor,
        mockProvider: hooks.mockProvider,
        onProcessChange: hooks.onProcessChange,
    });

    return {
        http,
        registry,
        supervisor,
        config,
        log,
        /** Bind the listener. */
        start: async () => {
            const address = await http.listen();
            bound = { host: address.host, port: address.port };
            return address;
        },
        /** Release listeners and owned resources, stopping workers first. */
        stop: async () => {
            const stopped = await supervisor.stopAll();
            confirmations.close();
            workerEvents.close();
            await http.close();
            return stopped;
        },
    };
}
