/** Skill reads share the existing authenticated panel HTTP boundary. */
import { sendJson } from '../http/server.ts';
import type { RouteHandler } from '../http/router.ts';
import { SkillStore } from './store.ts';

export function skillRoutes(store: SkillStore): Record<string, RouteHandler> {
    return {
        'GET /api/skills': async ({ res }) => sendJson(res, 200, await store.list()),
        'GET /api/skills/template': async ({ res }) => sendJson(res, 200, { text: await store.template() }),
        'GET /api/skills/:id': async ({ res, params }) => sendJson(res, 200, await store.read(params.id!)),
    };
}
