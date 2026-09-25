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
 */
import { join } from 'node:path';
import { normalizeSpec } from './spec.js';

/** Directory holding one session's generated files. */
export function sessionDir(config, sessionId) {
    return join(config.dataDir, 'workers', sessionId);
}

/** Path of the generated worker configuration for a session. */
export function workerConfigPath(config, sessionId) {
    return join(sessionDir(config, sessionId), 'config.yaml');
}

/** Root directory of worker session snapshots (`<dir>/<session>/state.json`). */
export function persistenceRoot(config) {
    return join(config.dataDir, 'sessions');
}

/**
 * Build the worker configuration document for one session.
 *
 * @param {object} options
 * @param {object} options.config hub configuration.
 * @param {string} options.sessionId session identifier.
 * @param {object} options.spec normalized session spec.
 * @param {{events: string, confirm: string}} options.endpoints worker-facing URLs.
 * @param {{baseUrl?: string}} [options.mock] resolved mock provider address.
 * @returns {object} the document to serialize as `config.yaml`.
 */
export function renderWorkerConfig({ config, sessionId, spec, endpoints, mock }) {
    const profileName = spec.provider;
    const profile = structuredClone(config.providerProfiles[profileName]);
    if (spec.model) profile.model = spec.model;
    if (mock?.baseUrl && profileName === config.mock.profile) {
        profile.endpoint = { ...(profile.endpoint ?? {}), base_url: mock.baseUrl };
    }

    const document = {
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
    return document;
}

/**
 * Convenience wrapper: normalize a stored spec and render the document.
 *
 * @returns {{spec: object, document: object}}
 */
export function renderSessionConfig({ config, sessionId, rawSpec, endpoints, mock }) {
    const spec = normalizeSpec(config, rawSpec);
    const document = renderWorkerConfig({ config, sessionId, spec, endpoints, mock });
    return { spec, document };
}
