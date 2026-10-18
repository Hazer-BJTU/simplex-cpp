import { PANEL_PROTOCOL, PANEL_VERSION, REST_ROUTES } from '@hazer-bjtu/simplex-hub/ui';
import type { PanelMessage, HubMessage, AnswerPage, ConfigFile, RestOperations, CreateSessionRequest, SessionSpec } from '@hazer-bjtu/simplex-hub/ui';
export const subscription: PanelMessage = { type: 'subscribe', session: 'example', paged: true, request_id: 'read-1' };
export type Replies = HubMessage | AnswerPage | ConfigFile;
export type Create = RestOperations['POST /api/sessions']['body'];
export const metadata = { protocol: PANEL_PROTOCOL, version: PANEL_VERSION, routes: REST_ROUTES };
// @ts-expect-error a fingerprint is mandatory for exact-answer correlation
const invalid: RestOperations['POST /api/sessions/:id/answer']['body'] = { source: { worker_id: 'w', turn: 0, step: 0, commit_sequence: '1' }, part: 0, offset: 0 };
// @ts-expect-error private implementation modules are not package entry points
import type { HubConfig } from '@hazer-bjtu/simplex-hub/dist/src/config.js';
// @ts-expect-error no root-level server API
import type { Server } from '@hazer-bjtu/simplex-hub';

export const json: CreateSessionRequest = { session: 'keep-json', spec: { persistence: { backend: 'json' } } };
export const sqlite: SessionSpec = { persistence: { backend: 'sqlite' } };
export const sqliteCreate: PanelMessage = { type: 'create_session', session: 'sqlite', spec: sqlite };
// @ts-expect-error only supported worker backends are public
const invalidBackend: CreateSessionRequest = { session: 'bad', spec: { persistence: { backend: 'mysql' } } };
// @ts-expect-error WebSocket creation shares the same backend contract
const invalidSocket: PanelMessage = { type: 'create_session', session: 'bad', spec: { persistence: { backend: 'mysql' } } };
