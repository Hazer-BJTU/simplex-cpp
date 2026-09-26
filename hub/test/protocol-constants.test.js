/**
 * @file the panel protocol version, and the guard that keeps it single-sourced.
 *
 * `shared/protocol.ts` is intended to be the only place a version is written
 * down, but the migration has not reached the three modules that carry their
 * own copies yet. This test is what makes that intermediate state safe: it
 * imports all four and fails when they disagree, so the drift the shared module
 * was created to prevent cannot happen while the wiring is still outstanding.
 *
 * It also exercises the interop the migration depends on: a `.js` test module
 * importing a `.ts` one, which works because Node strips types at load time.
 */
import assert from 'node:assert/strict';
import { before, describe, it } from 'node:test';
import { PANEL_PROTOCOL as HUB_PROTOCOL, CAPABILITIES as HUB_CAPABILITIES } from '../src/hub.js';
import { PANEL_VERSION as SERVER_PANEL_VERSION } from '../src/panel/api.js';
import { PANEL_VERSION as WEB_PANEL_VERSION } from '../web/js/api.js';

/**
 * True when Node loads a `.ts` module without a flag.
 *
 * Type stripping arrived experimentally in 22.6 and became the default in
 * 22.18. The hub itself still runs on 20.11 — nothing under `src/` imports
 * `shared/` yet, so an operator on the declared floor is unaffected — but a
 * *test* that loads a `.ts` module cannot run there. Skipping with the reason
 * stated is honest about that. It stops being acceptable the moment `src/`
 * imports `shared/`, at which point the declared floor has to move.
 */
function supportsTypeStripping() {
    const [major = 0, minor = 0] = process.versions.node.split('.').map(Number);
    return major > 22 || (major === 22 && minor >= 18);
}

/** The shared module, loaded lazily — see the note in the describe block. */
let shared = null;

describe('panel protocol constants', {
    skip: supportsTypeStripping()
        ? false
        : `type stripping needs Node >= 22.18; this is ${process.versions.node}`,
}, () => {
    // A static import would fail while this module is being loaded, which is
    // before `skip` is ever consulted — the whole file would error out instead
    // of skipping. Loading it inside the suite is what makes the guard real.
    before(async () => {
        shared = await import('../shared/protocol.ts');
    });

    it('reports one version from the shared module', () => {
        assert.equal(shared.PANEL_VERSION, shared.PANEL_PROTOCOL.version);
        assert.equal(shared.PANEL_PROTOCOL.name, 'simplex-hub-panel');
    });

    it('agrees with the hub metadata', () => {
        assert.equal(HUB_PROTOCOL.name, shared.PANEL_PROTOCOL.name);
        assert.equal(HUB_PROTOCOL.version, shared.PANEL_VERSION,
            'src/hub.js announces a protocol version the shared module does not');
    });

    it('agrees with the version stamped on every panel message', () => {
        assert.equal(SERVER_PANEL_VERSION, shared.PANEL_VERSION,
            'src/panel/api.js stamps a version the shared module does not');
        assert.equal(WEB_PANEL_VERSION, shared.PANEL_VERSION,
            'web/js/api.js stamps a version the shared module does not');
    });

    it('agrees with the capability list the hub reports', () => {
        assert.deepEqual([...HUB_CAPABILITIES].sort(), [...shared.CAPABILITIES].sort(),
            'src/hub.js advertises capabilities the shared module does not list');
    });
});
