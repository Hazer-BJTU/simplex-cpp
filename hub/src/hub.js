/**
 * @file hub assembly: wires the HTTP front door and every role adapter.
 *
 * One HTTP server carries all three audiences: the browser panel, the JSON API,
 * and the worker-facing WebSocket routes. Each role registers itself here, so
 * this file is the map of what the hub currently implements.
 */
import { createHttpServer, sendJson } from './http/server.js';
import { SessionRegistry } from './state/registry.js';
import { createWorkerConfirmationRoute } from './worker/confirmation.js';
import { createWorkerEventRoute } from './worker/connection.js';

/** Protocol name/version announced by `/api/meta`. */
export const PANEL_PROTOCOL = { name: 'simplex-hub-panel', version: 1 };

/** Capabilities reported by `/api/meta`; extended as roles are implemented. */
export const CAPABILITIES = ['worker-events', 'confirmations'];

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
 */
export function createHub({ config, log, hubRoot, version = '0.0.0', hooks = {} }) {
    const http = createHttpServer({ config, log, hubRoot });
    const registry = new SessionRegistry({ config, log });

    http.route('GET', '/api/meta', ({ res }) => {
        sendJson(res, 200, {
            name: 'simplex-hub',
            version,
            protocol: PANEL_PROTOCOL,
            worker_protocol: 'core/docs/worker-protocol.md',
            capabilities: CAPABILITIES,
            listen: { host: config.listen.host, port: config.listen.port },
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

    return {
        http,
        registry,
        config,
        log,
        /** Bind the listener. */
        start: () => http.listen(),
        /** Release listeners and owned resources. */
        stop: async () => {
            confirmations.close();
            workerEvents.close();
            await http.close();
        },
    };
}
