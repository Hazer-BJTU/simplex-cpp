import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { authorizePanel, cookieValue } from '../src/http/auth.ts';

describe('panel cookie parsing', () => {
    it('treats malformed percent encoding as an absent cookie', () => {
        const req = { headers: { cookie: 'simplex_hub_token=%ZZ', authorization: 'Bearer good' } };
        assert.equal(cookieValue(req, 'simplex_hub_token'), '');
        assert.deepEqual(authorizePanel({ panel: { token: 'good' } }, req, null), { ok: true });
    });
});
