/**
 * @file hub assembly: wires the HTTP front door and every role adapter.
 *
 * One HTTP server carries all three audiences: the browser panel, the JSON API,
 * and the worker-facing WebSocket routes. Each role registers itself here, so
 * this file is the map of what the hub currently implements — and the only
 * place that knows the whole object graph.
 */
import { authorizePanel } from './http/auth.js';
import { createHttpServer, sendError, sendJson } from './http/server.js';
import { createLauncher } from './launch/launcher.js';
import { MockProvider, parseAddress } from './mock/provider.js';
import { WorkerSupervisor } from './launch/supervisor.js';
import { createPanelApi } from './panel/api.js';
import { HubState } from './state/persist.js';
import { SessionRegistry } from './state/registry.js';
import { isValidSessionId } from './state/session-id.js';
import { TranscriptStore } from './state/transcript.js';
import { createWorkerConfirmationRoute } from './worker/confirmation.js';
import { createWorkerEventRoute } from './worker/connection.js';

/** Protocol name/version announced by `/api/meta`. */
export const PANEL_PROTOCOL = { name: 'simplex-hub-panel', version: 1 };

/** Capabilities reported by `/api/meta`; extended as roles are implemented. */
export const CAPABILITIES = [
    'worker-events',
    'confirmations',
    'supervisor',
    'transcript-replay',
    'snapshot-view',
];

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
 * @param {object} [options.hooks] extra observers (used by tests and the mock).
 * @param {() => ({baseUrl: string}|null)} [options.hooks.mockProvider]
 */
export function createHub({ config, log, hubRoot, version = '0.0.0', hooks: extraHooks = {} }) {
    const http = createHttpServer({ config, log, hubRoot });
    const registry = new SessionRegistry({ config, log });
    const transcripts = new TranscriptStore({ config, log });
    const state = new HubState({ config, log });
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

    /** Metadata shared by `/api/meta` and the panel's `welcome` message. */
    function meta() {
        return {
            name: 'simplex-hub',
            version,
            protocol: PANEL_PROTOCOL,
            worker_protocol: 'core/docs/worker-protocol.md',
            capabilities: CAPABILITIES,
            // The bound address once listening, so a client is not told the
            // configured port when the configuration asked for 0.
            listen: bound
                ? { host: bound.host, port: bound.port }
                : { host: config.listen.host, port: config.listen.port },
            launcher: {
                kind: config.launcher.kind,
                owns_config: config.launcher.config === 'launcher',
            },
            provider_profiles: Object.keys(config.providerProfiles),
            force_kill_process_group: config.forceKillProcessGroup,
            mock: { enabled: config.mock.enabled },
        };
    }

    /** Panel API first: the worker routes report through its hooks. */
    let panel = null;

    /**
     * Run one observer without letting its failure reach the caller.
     *
     * These hooks fan out from socket callbacks. A hook that throws would
     * otherwise become an uncaught exception in the middle of the worker's own
     * message handling, so each one is contained separately: a broken panel
     * observer must not stop the hub from recording an event, and must not stop
     * the other observers either.
     */
    const safely = (label, run) => {
        try {
            run();
        } catch (error) {
            log.error(`${label} hook failed: ${error.message}`, error);
        }
    };

    const workerEvents = createWorkerEventRoute({
        registry,
        config,
        log,
        onEvent: (envelope, connection) => {
            connection.log.debug(`event ${envelope.event} (seq ${envelope.sequence})`);
            safely('panel onEvent', () => panel?.hooks.onEvent(envelope, connection));
            safely('extra onEvent', () => extraHooks.onEvent?.(envelope, connection));
        },
        onConnectionChange: (session, connection) => {
            safely('panel onConnectionChange',
                () => panel?.hooks.onConnectionChange(session, connection));
            safely('extra onConnectionChange',
                () => extraHooks.onConnectionChange?.(session, connection));
        },
    });
    http.useUpgrade(workerEvents);

    const confirmations = createWorkerConfirmationRoute({
        registry,
        config,
        log,
        onPrompt: (prompt) => {
            safely('panel onPrompt', () => panel?.hooks.onPrompt(prompt));
            safely('extra onPrompt', () => extraHooks.onPrompt?.(prompt));
        },
        onSettled: (prompt, outcome) => {
            safely('panel onPromptSettled', () => panel?.hooks.onPromptSettled(prompt, outcome));
            safely('extra onPromptSettled', () => extraHooks.onPromptSettled?.(prompt, outcome));
        },
    });
    http.useUpgrade(confirmations);

    /** Started by `start()` when configured; read lazily by the supervisor. */
    let mock = null;
    const supervisor = new WorkerSupervisor({
        config,
        log,
        registry,
        launcher: createLauncher({ config, log }),
        endpointsFor,
        mockProvider: () => (mock ? { baseUrl: mock.baseUrl } : null),
        onProcessChange: (session, record) => {
            safely('panel onProcessChange', () => panel?.hooks.onProcessChange(session, record));
            safely('extra onProcessChange', () => extraHooks.onProcessChange?.(session, record));
        },
    });

    panel = createPanelApi({
        config,
        log,
        registry,
        supervisor,
        transcripts,
        state,
        meta,
        onSessionsChanged: (sessions) => state.schedule(sessions),
    });
    http.useUpgrade(panel.upgrade);

    /** Register the REST API behind panel authentication. */
    for (const [route, handler] of Object.entries(panel.routes)) {
        const [method, pattern] = route.split(' ');
        http.route(method, pattern, async (context) => {
            if (!authorizePanel(config, context.req, context.url).ok) {
                sendError(context.res, 401, 'unauthorized', 'a panel token is required');
                return;
            }
            await handler(context);
        });
    }

    http.route('GET', '/api/meta', ({ res }) => sendJson(res, 200, meta()));

    return {
        http,
        registry,
        supervisor,
        transcripts,
        panel,
        config,
        log,
        /** The offline provider, when enabled and started. */
        get mock() {
            return mock;
        },
        /** Bind the listener and restore sessions recorded by a previous run. */
        start: async () => {
            const address = await http.listen();
            bound = { host: address.host, port: address.port };
            if (config.mock.enabled) {
                mock = new MockProvider({
                    log: log.child('mock'),
                    scenario: config.mock.scenario,
                    slowMs: config.mock.slowMs,
                });
                await mock.start(parseAddress(config.mock.listen));
            }
            const stored = state.load();
            let restored = 0;
            for (const entry of stored.sessions ?? []) {
                if (!isValidSessionId(entry?.id) || registry.get(entry.id)) continue;
                const session = registry.create(entry.id, entry.spec ?? {});
                // Tokens must survive a restart, or a worker that is still
                // running would be locked out by its own hub.
                if (typeof entry.token === 'string' && entry.token.length > 0) {
                    session.token = entry.token;
                }
                if (typeof entry.created_at === 'string') session.createdAt = entry.created_at;
                session.spec = entry.spec ?? {};
                if (entry.process) supervisor.adopt(session, entry.process);
                restored += 1;
            }
            if (restored > 0) log.info(`restored ${restored} session(s) from ${state.path}`);
            state.schedule(registry.list());
            return address;
        },
        /** Release listeners and owned resources, stopping workers first. */
        stop: async () => {
            const stopped = await supervisor.stopAll();
            state.flush(registry.list());
            await mock?.stop();
            panel.close();
            confirmations.close();
            workerEvents.close();
            await http.close();
            transcripts.close();
            return stopped;
        },
    };
}
