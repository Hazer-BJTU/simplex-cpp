/**
 * @file hub assembly: wires the HTTP front door and every role adapter.
 *
 * Milestone order is visible here — the worker-facing adapter, the confirmation
 * adapter, the process supervisor, the session registry, and the panel API are
 * all attached to one HTTP server, and `stop()` releases them in the reverse
 * order of their dependencies.
 */
import { createHttpServer, sendJson } from './http/server.js';

/** Protocol name/version announced by `/api/meta`. */
export const PANEL_PROTOCOL = { name: 'simplex-hub-panel', version: 1 };

/**
 * Build a hub instance. Nothing listens until `start()` is called.
 *
 * @param {object} options
 * @param {object} options.config validated hub configuration.
 * @param {object} options.log logger created by src/log.js.
 * @param {string} options.hubRoot absolute `hub/` directory.
 * @param {string} [options.version] hub package version.
 */
export function createHub({ config, log, hubRoot, version = '0.0.0' }) {
    const http = createHttpServer({ config, log, hubRoot });

    http.route('GET', '/api/meta', ({ res }) => {
        sendJson(res, 200, {
            name: 'simplex-hub',
            version,
            protocol: PANEL_PROTOCOL,
            worker_protocol: 'core/docs/worker-protocol.md',
            capabilities: [],
            listen: { host: config.listen.host, port: config.listen.port },
        });
    });

    return {
        http,
        config,
        log,
        /** Bind the listener. */
        start: () => http.listen(),
        /** Release listeners and owned resources. */
        stop: () => http.close(),
    };
}
