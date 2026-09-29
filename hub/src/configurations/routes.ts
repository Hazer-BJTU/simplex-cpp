/** Configuration APIs share the panel's authentication and authority. */
import { readJsonBody, sendJson } from '../http/server.ts';
import type { IncomingMessage } from 'node:http';
import type { RouteHandler } from '../http/router.ts';
import { ConfigurationStore, launchDocument, configError } from './store.ts';
import type { ConfigKind } from './store.ts';
import type { WorkerSupervisor } from '../launch/supervisor.ts';
import { launchEndpoints } from './session.ts';

/** Require an object body before reading fields, including for malformed clients. */
async function objectBody(req: IncomingMessage, limit: number): Promise<Record<string, unknown>> {
    const body = await readJsonBody(req, limit);
    if (!body || typeof body !== 'object' || Array.isArray(body)) throw configError('Request body must be an object');
    return body as Record<string, unknown>;
}

export function configurationRoutes(store: ConfigurationStore, supervisor: WorkerSupervisor): Record<string, RouteHandler> {
    return {
        'GET /api/configurations': ({ res }) => {
            sendJson(res, 200, { launch: store.list('launch'), worker: store.list('worker') });
        },
        'GET /api/configurations/:kind/template': ({ res, params, url }) => {
            const kind = params.kind as ConfigKind;
            store.path(kind, 'template');
            const source = url.searchParams.get('source') ?? 'default';
            if (!['default', 'deployment', 'docker'].includes(source)) throw configError('Unknown template source');
            sendJson(res, 200, { text: store.template(kind, source as 'default' | 'deployment' | 'docker') });
        },
        'POST /api/configurations/preview': async ({ req, res }) => {
            const body = await objectBody(req, 1024 * 1024) as { launch: string };
            const launch = launchDocument(body.launch, store.config);
            const endpoints = launchEndpoints(supervisor.endpointsFor('SESSION_ID', 'SESSION_TOKEN'), launch);
            sendJson(res, 200, { endpoints });
        },
        'POST /api/configurations/:kind/validate': async ({ req, res, params }) => {
            const body = await objectBody(req, 1024 * 1024) as { text: string };
            store.validate(params.kind as ConfigKind, body.text);
            sendJson(res, 200, { valid: true });
        },
        'GET /api/configurations/:kind/:id': ({ res, params }) => {
            sendJson(res, 200, store.read(params.kind as ConfigKind, params.id!));
        },
        'PUT /api/configurations/:kind/:id': async ({ req, res, params }) => {
            const body = await objectBody(req, 1024 * 1024) as { text: string; revision: string | null };
            if (body.revision !== null && typeof body.revision !== 'string') throw configError('revision is required (null for a new file)');
            sendJson(res, 200, store.save(params.kind as ConfigKind, params.id!, body.text, body.revision));
        },
        'DELETE /api/configurations/:kind/:id': async ({ req, res, params }) => {
            const body = await objectBody(req, 1024) as { revision: string };
            store.remove(params.kind as ConfigKind, params.id!, body.revision);
            sendJson(res, 200, { removed: params.id });
        },
        'POST /api/configurations/:kind/:id/rename': async ({ req, res, params }) => {
            const body = await objectBody(req, 1024) as { id: string; revision: string };
            const original = store.read(params.kind as ConfigKind, params.id!);
            if (original.revision !== body.revision) throw configError('Configuration changed; reload before renaming', 409);
            const saved = store.save(original.kind, body.id, original.text, null);
            store.remove(original.kind, original.id, original.revision);
            sendJson(res, 200, saved);
        },
    };
}
