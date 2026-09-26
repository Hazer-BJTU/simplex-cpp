/**
 * @file the panel protocol version, and the guard that keeps it single-sourced.
 *
 * `shared/protocol.ts` is the only place a version is written down, and this is
 * what keeps it that way: the two modules that still carry their own constant
 * are imported here and fail the moment they disagree.
 *
 * There were three copies during the rewrite — the hub's, the panel API's, and
 * the old build-free panel's `web/js/api.js`. The third went away with that
 * panel (P8), which is the reason the count in this file is now two rather than
 * a reason to relax it.
 *
 * It also exercises the interop the migration depends on: a `.js` test module
 * importing a `.ts` one, which works because Node strips types at load time.
 * That is why the declared floor is 22.18 — before it, this file would not load
 * at all, and there is deliberately no skip for it now.
 */
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';
import { describe, it } from 'node:test';
import { hubRoot } from '../src/config.ts';
import { PANEL_PROTOCOL as HUB_PROTOCOL, CAPABILITIES as HUB_CAPABILITIES } from '../src/hub.ts';
import { PANEL_VERSION as SERVER_PANEL_VERSION } from '../src/panel/api.ts';
import {
    CAPABILITIES as SHARED_CAPABILITIES,
    PANEL_PROTOCOL as SHARED_PROTOCOL,
    PANEL_VERSION as SHARED_VERSION,
} from '../shared/protocol.ts';

describe('panel protocol constants', () => {
    it('reports one version from the shared module', () => {
        assert.equal(SHARED_VERSION, SHARED_PROTOCOL.version);
        assert.equal(SHARED_PROTOCOL.name, 'simplex-hub-panel');
    });

    it('agrees with the hub metadata', () => {
        assert.equal(HUB_PROTOCOL.name, SHARED_PROTOCOL.name);
        assert.equal(HUB_PROTOCOL.version, SHARED_VERSION,
            'src/hub.js announces a protocol version the shared module does not');
    });

    it('agrees with the version stamped on every panel message', () => {
        assert.equal(SERVER_PANEL_VERSION, SHARED_VERSION,
            'src/panel/api.ts stamps a version the shared module does not');
    });

    it('is the version the browser half reads rather than repeats', () => {
        // The panel used to carry its own literal. The check that replaces it is
        // structural: nothing under `web/src` may write a version down, so the
        // only way it can drift again is by importing something else.
        const sources = [];
        (function walk(directory) {
            for (const entry of readdirSync(directory, { withFileTypes: true })) {
                const path = join(directory, entry.name);
                if (entry.isDirectory()) walk(path);
                else if (/\.tsx?$/.test(entry.name)) sources.push(path);
            }
        })(join(hubRoot, 'web', 'src'));
        const offenders = sources
            .filter((path) => /version\s*[:=]\s*['"]?\d/.test(readFileSync(path, 'utf8')))
            .map((path) => path.slice(hubRoot.length + 1));
        assert.deepEqual(offenders, [],
            'a panel module writes a protocol version down instead of importing it');
    });

    it('agrees with the capability list the hub reports', () => {
        assert.deepEqual([...HUB_CAPABILITIES].sort(), [...SHARED_CAPABILITIES].sort(),
            'src/hub.js advertises capabilities the shared module does not list');
    });
});
