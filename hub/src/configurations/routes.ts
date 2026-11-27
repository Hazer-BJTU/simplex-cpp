import type { ConfigList, ConfigTemplateResponse, ConfigPreviewResponse, ConfigValidateResponse, ConfigRemoveResponse } from '../../ui/configurations.ts';
/** Configuration APIs share the panel's authentication and authority. */
import { spawn } from 'node:child_process';
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

function stringField(body: Record<string, unknown>, key: string): string {
    if (typeof body[key] !== 'string') throw configError(`${key} must be a string`);
    return body[key];
}

// One UTF-8 source byte can occupy six JSON bytes when escaped as a Unicode control character.
const maxSourceBodyBytes = 6 * 1024 * 1024 + 1024;

export function configurationRoutes(store: ConfigurationStore, supervisor: WorkerSupervisor): Record<string, RouteHandler> {
    return {
        'GET /api/configurations': ({ res }) => {
            sendJson(res, 200, { launch: store.list('launch'), worker: store.list('worker'),
                ...(store.config.panel.editor ? { editorAvailable: true } : {}) } satisfies ConfigList);
        },
        'GET /api/configurations/:kind/template': ({ res, params, url }) => {
            const kind = params.kind as ConfigKind;
            store.path(kind, 'template');
            const source = url.searchParams.get('source') ?? 'default';
            if (!['default', 'docker', 'managed'].includes(source)) throw configError('Unknown template source');
            sendJson(res, 200, { text: store.template(kind, source as 'default' | 'docker' | 'managed') } satisfies ConfigTemplateResponse);
        },
        'POST /api/configurations/preview': async ({ req, res }) => {
            const body = await objectBody(req, maxSourceBodyBytes);
            const text = stringField(body, 'launch');
            if (Buffer.byteLength(text) > 1024 * 1024) throw configError('Configuration exceeds 1 MiB', 413);
            const launch = launchDocument(text, store.config);
            const endpoints = launchEndpoints(supervisor.endpointsFor('SESSION_ID', 'SESSION_TOKEN'), launch);
            sendJson(res, 200, { endpoints } satisfies ConfigPreviewResponse);
        },
        'POST /api/configurations/:kind/validate': async ({ req, res, params }) => {
            const body = await objectBody(req, maxSourceBodyBytes);
            store.validate(params.kind as ConfigKind, stringField(body, 'text'));
            sendJson(res, 200, { valid: true } satisfies ConfigValidateResponse);
        },
        'GET /api/configurations/:kind/:id/source': ({ res, params }) => {
            const file = store.read(params.kind as ConfigKind, params.id!);
            sendJson(res, 200, { ...file, path: store.path(file.kind, file.id) });
        },
        'GET /api/configurations/:kind/:id': ({ res, params }) => {
            sendJson(res, 200, store.read(params.kind as ConfigKind, params.id!));
        },
        'PUT /api/configurations/:kind/:id': async ({ req, res, params }) => {
            const body = await objectBody(req, maxSourceBodyBytes);
            if (body.revision !== null && typeof body.revision !== 'string') throw configError('revision is required (null for a new file)');
            sendJson(res, 200, store.save(params.kind as ConfigKind, params.id!,
                stringField(body, 'text'), body.revision));
        },
        'DELETE /api/configurations/:kind/:id': async ({ req, res, params }) => {
            const body = await objectBody(req, 1024);
            store.remove(params.kind as ConfigKind, params.id!, stringField(body, 'revision'));
            sendJson(res, 200, { removed: params.id! } satisfies ConfigRemoveResponse);
        },
        'POST /api/configurations/:kind/:id/open': async ({ req, res, params }) => {
            const editor = store.config.panel.editor;
            if (!editor) throw configError('No editor is configured on the Hub host', 409);
            if (req.headers['content-type']?.split(';')[0]?.trim() !== 'application/json') {
                throw configError('Expected application/json', 415);
            }
            if (Object.keys(await objectBody(req, 1024)).length) throw configError('Expected an empty object');
            const file = store.read(params.kind as ConfigKind, params.id!);
            const path = store.path(file.kind, file.id);
            const args = (editor.args ?? ['{file}']).map(arg => arg === '{file}' ? path : arg);
            // Report exec errors, but do not wait for a graphical editing session to finish.
            await new Promise<void>((resolve, reject) => {
                const child = spawn(editor.command, args, { stdio: 'ignore', detached: true, shell: false });
                child.once('error', () => reject(configError('Could not launch the configured Hub editor', 503)));
                child.once('spawn', () => { child.unref(); resolve(); });
            });
            sendJson(res, 200, { opened: true });
        },
        'POST /api/configurations/:kind/:id/rename': async ({ req, res, params }) => {
            const body = await objectBody(req, 1024);
            const id = stringField(body, 'id');
            const revision = stringField(body, 'revision');
            const original = store.read(params.kind as ConfigKind, params.id!);
            if (original.revision !== revision) throw configError('Configuration changed; reload before renaming', 409);
            const saved = store.save(original.kind, id, original.text, null);
            store.remove(original.kind, original.id, original.revision);
            sendJson(res, 200, saved);
        },
    };
}
