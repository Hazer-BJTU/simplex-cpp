/**
 * @file worker configuration rendering.
 *
 * The hub generates a complete worker configuration per session. It is written
 * as JSON: `load::read_configuration` parses YAML, and JSON is a YAML subset, so
 * the generated file needs no YAML serializer and cannot drift from what the
 * loader accepts. The in-tree container test writes its `config.yaml` the same
 * way.
 *
 * Credentials are never resolved here. A profile may contain `${NAME}`, which
 * the *worker* expands from its own environment at startup, so an API key does
 * not have to pass through the hub or reach the disk.
 *
 * `WorkerConfigDocument` below is a hand-maintained copy of the schema core
 * documents in `core/docs/worker-protocol.md`. It is the one interface here
 * whose other end is C++, so a change to it is a change to that contract.
 */
import { join } from 'node:path';
import { normalizeSpec } from './spec.ts';
import type { NormalizedSpec } from './spec.ts';
import type { HubConfig, ProviderProfile } from '../config.ts';
import type { WorkerEndpoints } from './invocation.ts';

/** The slice of hub configuration the renderer reads. */
export type RenderConfig = Pick<HubConfig, 'dataDir' | 'worker' | 'providerProfiles' | 'mock'>;

/** Directory holding one session's generated files. */
export function sessionDir(config: { dataDir: string }, sessionId: string): string {
    return join(config.dataDir, 'workers', sessionId);
}

/** Path of the generated worker configuration for a session. */
export function workerConfigPath(config: { dataDir: string }, sessionId: string): string {
    return join(sessionDir(config, sessionId), 'config.yaml');
}

/** Root directory of worker session snapshots (`<dir>/<session>/state.json`). */
export function persistenceRoot(config: { dataDir: string }): string {
    return join(config.dataDir, 'sessions');
}

/**
 * The worker configuration document.
 *
 * Snake case throughout: this is the C++ side's schema, not the hub's.
 */
export interface WorkerConfigDocument {
    plugins: {
        providers: { directories: string[] };
        extensions: {
            tools: { directories: string[]; enable: string[] };
            loop_hooks: { directories: string[]; enable: string[] };
        };
    };
    providers: Record<string, ProviderProfile>;
    driver_model: string;
    client: {
        endpoint: string;
        payload_capacity: number;
        signal_capacity: number;
        transport: {
            write_capacity: number;
            initial_backoff_ms: number;
            max_backoff_ms: number;
            idle_timeout_seconds: number;
        };
    };
    security: { confirmation: { endpoint: string; timeout_ms: number } };
    worker: {
        max_exchanges: number;
        event_capacity: number;
        system_prompt_file: string;
        environment: { workspace: string; platform: string; software: string[] };
    };
    persistence: {
        enabled: boolean;
        directory: string;
        format: string;
        readable: boolean;
        restore: string;
        save: { on_step_finished: boolean; on_run_finished: boolean; on_shutdown: boolean };
    };
}

/** What `renderWorkerConfig` accepts. */
export interface RenderWorkerConfigOptions {
    config: RenderConfig;
    sessionId: string;
    spec: NormalizedSpec;
    endpoints: WorkerEndpoints;
    /** Resolved mock provider address, when the mock provider is running. */
    mock?: { baseUrl: string } | null | undefined;
}

/**
 * Build the worker configuration document for one session.
 *
 * `sessionId` is accepted and not read: the document is a function of the
 * configuration and the spec, and the caller already knows which session it is
 * rendering for. It stays in the signature so the call sites read uniformly.
 */
export function renderWorkerConfig({
    config, spec, endpoints, mock,
}: RenderWorkerConfigOptions): WorkerConfigDocument {
    const profileName = spec.provider;
    const profile = structuredClone(config.providerProfiles[profileName]) as ProviderProfile;
    if (spec.model) profile.model = spec.model;
    if (mock?.baseUrl && profileName === config.mock.profile) {
        profile.endpoint = {
            ...(profile.endpoint as Record<string, unknown> ?? {}),
            base_url: mock.baseUrl,
        };
    }

    return {
        // Plugin discovery stays at its executable-relative default; dynamic
        // toolsets and loop hooks are not part of this contract.
        plugins: {
            providers: { directories: [] },
            extensions: {
                tools: { directories: [], enable: [] },
                loop_hooks: { directories: [], enable: [] },
            },
        },
        providers: { [profileName]: profile },
        driver_model: profileName,
        client: {
            endpoint: endpoints.events,
            payload_capacity: config.worker.payloadCapacity,
            signal_capacity: config.worker.signalCapacity,
            transport: {
                write_capacity: config.worker.writeCapacity,
                initial_backoff_ms: config.worker.initialBackoffMs,
                max_backoff_ms: config.worker.maxBackoffMs,
                idle_timeout_seconds: config.worker.idleTimeoutSeconds,
            },
        },
        security: {
            confirmation: {
                endpoint: endpoints.confirm,
                timeout_ms: config.worker.confirmationTimeoutMs,
            },
        },
        worker: {
            max_exchanges: spec.maxExchanges,
            event_capacity: spec.eventCapacity,
            system_prompt_file: spec.systemPromptFile,
            environment: {
                workspace: spec.workspace,
                platform: spec.platform,
                software: spec.software,
            },
        },
        persistence: {
            enabled: spec.persistence.enabled,
            directory: persistenceRoot(config),
            format: 'json',
            readable: spec.persistence.readable,
            restore: spec.restore,
            save: {
                on_step_finished: true,
                on_run_finished: true,
                on_shutdown: true,
            },
        },
    };
}

/** What `renderSessionConfig` accepts. */
export interface RenderSessionConfigOptions {
    config: RenderConfig;
    sessionId: string;
    /** The stored spec, unnormalized. */
    rawSpec: unknown;
    endpoints: WorkerEndpoints;
    mock?: { baseUrl: string } | null | undefined;
}

/** Convenience wrapper: normalize a stored spec and render the document. */
export function renderSessionConfig({
    config, sessionId, rawSpec, endpoints, mock,
}: RenderSessionConfigOptions): { spec: NormalizedSpec; document: WorkerConfigDocument } {
    const spec = normalizeSpec(config as HubConfig, rawSpec);
    const document = renderWorkerConfig({ config, sessionId, spec, endpoints, mock });
    return { spec, document };
}
