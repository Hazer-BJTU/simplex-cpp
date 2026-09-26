/**
 * @file hub panel protocol vocabulary, shared by the server and the panel.
 *
 * This module is the single place a panel-protocol version is written down. It
 * exists because the version is currently spelled out three times — in
 * `src/hub.js`, in `src/panel/api.js`, and in the browser's `web/js/api.js` —
 * and nothing compared them. `protocol.version` in `/api/meta` and the `v`
 * stamped on every frame could therefore drift apart without a test noticing,
 * which is a poor foundation for the additive changes the protocol is supposed
 * to support.
 *
 * It is TypeScript on purpose. Node runs this file directly (type stripping,
 * Node >= 22.18) and the panel bundle compiles the same source, so the two ends
 * cannot disagree about a constant or a message shape. Nothing here may import
 * a Node built-in or touch the DOM: the browser is one of its consumers.
 */

/** Protocol name and version announced by `/api/meta` and by `welcome`. */
export const PANEL_PROTOCOL = {
    name: 'simplex-hub-panel',
    version: 1,
} as const;

/**
 * Version stamped on every panel message.
 *
 * Derived rather than repeated, because a second literal is a second thing to
 * forget. `src/panel/api.js` and `web/js/api.js` still carry their own copies
 * during the migration; `test/protocol-constants.test.js` fails if any of the
 * three disagrees with this one.
 */
export const PANEL_VERSION: number = PANEL_PROTOCOL.version;

/**
 * Features a hub may advertise in `/api/meta`.
 *
 * The list is what makes an additive change safe: a panel asks for a capability
 * instead of guessing from a version number, and an older hub that does not
 * advertise it simply does not get that part of the UI. The values below are
 * the ones `src/hub.js` already reports — they were announced before anything
 * read them, which is why the panel never grew the check.
 */
export const CAPABILITIES = [
    'worker-events',
    'confirmations',
    'supervisor',
    'transcript-replay',
    'snapshot-view',
] as const;

/** One advertised capability. */
export type Capability = (typeof CAPABILITIES)[number];
