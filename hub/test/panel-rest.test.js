import assert from 'node:assert/strict';
import { test } from 'node:test';
import { createRest } from '../web/src/lib/rest.ts';

for (const result of [
    { ok: true, how: 'process-group', forced: true },
    { ok: false, how: 'still-running', forced: false },
    { ok: false, error: 'unknown session' },
]) {
    test(`force-kill preserves its HTTP 200 outcome: ${JSON.stringify(result)}`, async () => {
        const rest = createRest({ fetchImpl: async (url, init) => {
            assert.equal(new URL(url).pathname, '/api/sessions/worker%20one/force-kill');
            assert.equal(init.method, 'POST');
            // This route consumes no launch overrides or request body.
            assert.equal(init.body, undefined);
            return Response.json(result);
        } });
        assert.deepEqual(await rest.forceKill('worker one'), result);
    });
}
