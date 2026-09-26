/**
 * @file hub assembly: wires the HTTP front door and every role adapter.
 *
 * One HTTP server carries all three audiences: the browser panel, the JSON API,
 * and the worker-facing WebSocket routes. Each role registers itself here, so
 * this file is the map of what the hub currently implements — and the only
 * place that knows the whole object graph.
 */
import { randomUUID } from 'node:crypto';
import { authorizePanel } from './http/auth.ts';
import { createHttpServer, sendError, sendJson } from './http/server.ts';
import type { BoundAddress } from './http/server.ts';
import { createLauncher } from './launch/launcher.ts';
import { MockProvider, parseAddress } from './mock/provider.ts';
import { WorkerSupervisor } from './launch/supervisor.ts';
import type { ProcessRecord } from './launch/supervisor.ts';
import { createPanelApi } from './panel/api.ts';
import type { PanelApi } from './panel/api.ts';
import { HubState } from './state/persist.ts';
import { SessionRegistry } from './state/registry.ts';
import type { Session } from './state/registry.ts';
import { isValidSessionId } from './state/session-id.ts';
import { TranscriptStore } from './state/transcript.ts';
import { createWorkerConfirmationRoute } from './worker/confirmation.ts';
import type { PendingConfirmation } from './worker/confirmation.ts';
import { createWorkerEventRoute } from './worker/connection.ts';
import type { ForwardedEnvelope, WorkerConnection } from './worker/connection.ts';
import { CAPABILITIES, PANEL_PROTOCOL } from '../shared/protocol.ts';
import type { Capability, ConfirmationOutcome, HubMetadata, SessionSpec } from '../shared/protocol.ts';
import type { HubConfig } from './config.ts';
import type { Logger } from './log.ts';

// Re-exported so a consumer of the hub does not have to reach into the shared
// module for the two constants it is most likely to want. The definitions live
// in `shared/protocol.ts`, which is the point: one place, both ends.
export { CAPABILITIES, PANEL_PROTOCOL };

/**
 * Host a worker should connect back to.
 *
 * A wildcard listener must not be echoed into the worker's configuration: the
 * worker needs a routable address, and loopback is the only one the hub can
 * assume for a service that listens on every interface.
 */
export function connectHostFor(host: string): string {
    if (host === '0.0.0.0' || host === '::' || host === '') return '127.0.0.1';
    return host;
}

/**
 * Extra observers, used by tests and by anything that embeds the hub.
 *
 * Every one is optional and every one is called through this hub's own
 * containment, so a hook that throws cannot reach the socket callback that
 * emitted the event.
 */
export interface HubHooks {
    onEvent?: ((envelope: ForwardedEnvelope, connection: WorkerConnection) => void) | undefined;
    onConnectionChange?:
        ((session: Session, connection: WorkerConnection | null) => void) | undefined;
    onPrompt?: ((prompt: PendingConfirmation) => void) | undefined;
    onPromptSettled?:
        ((prompt: PendingConfirmation, outcome: ConfirmationOutcome) => void) | undefined;
    onProcessChange?: ((session: Session, record: ProcessRecord) => void) | undefined;
}

/** Everything `createHub` needs. */
export interface CreateHubOptions {
    /** Validated hub configuration. */
    config: HubConfig;
    log: Logger;
    /** Absolute `hub/` directory. */
    hubRoot: string;
    version?: string;
    hooks?: HubHooks;
}

/** The hub instance. Nothing listens until `start()` is called. */
export interface Hub {
    http: ReturnType<typeof createHttpServer>;
    registry: SessionRegistry;
    supervisor: WorkerSupervisor;
    transcripts: TranscriptStore;
    panel: PanelApi;
    config: HubConfig;
    log: Logger;
    /** The offline provider, when enabled and started. */
    readonly mock: MockProvider | null;
    start(): Promise<BoundAddress>;
    stop(): Promise<unknown>;
}

/** Build a hub instance. */
export function createHub({
    config, log, hubRoot, version = '0.0.0', hooks: extraHooks = {},
}: CreateHubOptions): Hub {
    const http = createHttpServer({ config, log, hubRoot });
    const registry = new SessionRegistry({ config, log });
    const transcripts = new TranscriptStore({ config, log });
    const state = new HubState({ config, log });
    /** Bound address, known only after `start()`. */
    let bound: { host: string; port: number } | null = null;

    /**
     * The address a worker should use to reach this hub.
     *
     * `worker.connectHost` wins when it is set: a hub that listens on every
     * interface, or behind a bridge, has no way to guess the address that works
     * from the worker's side, and the loopback default is actively wrong there.
     */
    function advertisedHost(): string {
        if (!bound) throw new Error('the hub is not listening yet');
        return config.worker.connectHost || connectHostFor(bound.host);
    }

    /** Worker-facing URLs for one session, valid after the listener is bound. */
    function endpointsFor(sessionId: string, token: string): { events: string; confirm: string } {
        if (!bound) throw new Error('the hub is not listening yet');
        const host = advertisedHost();
        const authority = `${host.includes(':') ? `[${host}]` : host}:${bound.port}`;
        const query = `token=${encodeURIComponent(token)}`;
        return {
            events: `ws://${authority}/agent/${sessionId}/events?${query}`,
            confirm: `ws://${authority}/agent/${sessionId}/confirm?${query}`,
        };
    }

    /**
     * Identifies this hub process's transcript.
     *
     * `hub_sequence` counts the envelopes *this* process received, so it starts
     * again at 1 after a restart. A replay cursor taken before one would then
     * silently return an empty transcript — indistinguishable from an idle
     * session. Publishing an epoch is what lets a client tell the two apart.
     */
    const transcriptEpoch = randomUUID();

    /** Metadata shared by `/api/meta` and the panel's `welcome` message. */
    function meta(): HubMetadata {
        return {
            name: 'simplex-hub',
            version,
            protocol: PANEL_PROTOCOL,
            worker_protocol: 'core/docs/worker-protocol.md',
            // A copy per response: the list is a module constant, and handing
            // the same array to every caller lets one of them mutate it for all.
            capabilities: [...CAPABILITIES] as Capability[],
            transcript_epoch: transcriptEpoch,
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
    let panel: PanelApi | null = null;

    /**
     * Run one observer without letting its failure reach the caller.
     *
     * These hooks fan out from socket callbacks. A hook that throws would
     * otherwise become an uncaught exception in the middle of the worker's own
     * message handling, so each one is contained separately: a broken panel
     * observer must not stop the hub from recording an event, and must not stop
     * the other observers either.
     */
    const safely = (label: string, run: () => void): void => {
        try {
            run();
        } catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            log.error(`${label} hook failed: ${message}`, error);
        }
    };

    const workerEvents = createWorkerEventRoute({
        registry,
        config,
        log,
        onEvent: (envelope: ForwardedEnvelope, connection: WorkerConnection) => {
            connection.log.debug(`event ${envelope.event} (seq ${String(envelope.sequence)})`);
            safely('panel onEvent', () => panel?.hooks.onEvent(envelope, connection));
            safely('extra onEvent', () => extraHooks.onEvent?.(envelope, connection));
        },
        onConnectionChange: (session: Session, connection: WorkerConnection | null) => {
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
        onPrompt: (prompt: PendingConfirmation) => {
            safely('panel onPrompt', () => panel?.hooks.onPrompt(prompt));
            safely('extra onPrompt', () => extraHooks.onPrompt?.(prompt));
        },
        onSettled: (prompt: PendingConfirmation, outcome: ConfirmationOutcome) => {
            safely('panel onPromptSettled', () => panel?.hooks.onPromptSettled(prompt, outcome));
            safely('extra onPromptSettled', () => extraHooks.onPromptSettled?.(prompt, outcome));
        },
    });
    http.useUpgrade(confirmations);

    /** Started by `start()` when configured; read lazily by the supervisor. */
    let mock: MockProvider | null = null;
    const supervisor = new WorkerSupervisor({
        config,
        log,
        registry,
        launcher: createLauncher({ config, log }),
        endpointsFor,
        // Only once the mock listener is actually bound: a base URL of null
        // would be written into the worker configuration as `null`.
        mockProvider: () => (mock?.baseUrl ? { baseUrl: mock.baseUrl } : null),
        onProcessChange: (session: Session, record: ProcessRecord) => {
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
        const [method, pattern] = route.split(' ') as [string, string];
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
        get mock(): MockProvider | null {
            return mock;
        },
        /** Bind the listener and restore sessions recorded by a previous run. */
        start: async (): Promise<BoundAddress> => {
            const address = await http.listen();
            bound = { host: address.host, port: address.port };
            if (config.mock.enabled) {
                mock = new MockProvider({
                    log: log.child('mock'),
                    scenario: config.mock.scenario,
                    slowMs: config.mock.slowMs,
                    ...(config.mock.toolCommand ? { toolCommand: config.mock.toolCommand } : {}),
                });
                await mock.start(parseAddress(config.mock.listen),
                    // The provider is reached by the worker, so it has to be
                    // advertised at the same address the hub is.
                    { advertiseHost: config.worker.connectHost });
            }
            const stored = state.load();
            let restored = 0;
            for (const raw of stored.sessions) {
                // Entries come from a file that may have been edited by hand, so
                // each one is narrowed here rather than trusted from the type.
                if (typeof raw !== 'object' || raw === null) continue;
                const entry = raw as Record<string, unknown>;
                if (!isValidSessionId(entry.id) || registry.get(entry.id)) continue;
                const session = registry.create(entry.id, (entry.spec ?? {}) as SessionSpec);
                // Tokens must survive a restart, or a worker that is still
                // running would be locked out by its own hub.
                if (typeof entry.token === 'string' && entry.token.length > 0) {
                    session.token = entry.token;
                }
                if (typeof entry.created_at === 'string') session.createdAt = entry.created_at;
                session.spec = (entry.spec ?? {}) as SessionSpec;
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
