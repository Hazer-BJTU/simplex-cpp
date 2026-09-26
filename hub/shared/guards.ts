/**
 * @file runtime checks for the panel protocol envelope.
 *
 * The types in `protocol.ts` describe what a message looks like; nothing at
 * runtime enforces them, because the hub is JavaScript and the wire is JSON.
 * This module is that enforcement, kept deliberately narrow: it validates the
 * *envelope* — that the text parses, that it is an object, that `v` is the
 * version this hub speaks, that `type` is one it knows.
 *
 * What it does not do is check the body of a message. Whether a `session`
 * exists, whether a spec is usable, whether a confirmation is still open: those
 * depend on hub state, not on the protocol, and they stay where that state
 * lives. Splitting it this way is what lets the same module run in the browser,
 * where there is no hub state to consult.
 *
 * Unknown message *types* are reported rather than rejected, because the
 * protocol's forward-compatibility rule is that an unfamiliar type is ignored.
 * The caller decides what to log; nothing here decides policy.
 */
import {
    PANEL_MESSAGE_TYPES,
    PANEL_VERSION,
    type ErrorCode,
    type PanelMessage,
    type PanelMessageType,
} from './protocol.ts';

/** Outcome of checking one inbound panel message. */
export type EnvelopeCheck =
    /** A known message type with a valid envelope. The body is still unchecked. */
    | { kind: 'ok'; message: PanelMessage }
    /** The type is not one this protocol version defines: ignore it. */
    | { kind: 'unknown_type'; type: string }
    /** The envelope itself is unusable; answer with `code`. */
    | { kind: 'rejected'; code: ErrorCode; detail: string };

/** True when `type` is a message type this protocol version defines. */
export function isPanelMessageType(type: unknown): type is PanelMessageType {
    return typeof type === 'string'
        && (PANEL_MESSAGE_TYPES as readonly string[]).includes(type);
}

/**
 * Parse and check one inbound panel message.
 *
 * @param text the frame as text. The caller decodes bytes; a binary frame is
 *   refused before it gets here, with its own error code.
 */
export function checkEnvelope(text: string): EnvelopeCheck {
    let parsed: unknown;
    try {
        parsed = JSON.parse(text);
    } catch (error) {
        return {
            kind: 'rejected',
            code: 'bad_json',
            detail: error instanceof Error ? error.message : 'not JSON',
        };
    }
    // A JSON array reaches the type check below with `type === undefined` and is
    // reported as an unknown type, which is what this hub has always done.
    if (typeof parsed !== 'object' || parsed === null) {
        return { kind: 'rejected', code: 'bad_message', detail: 'expected a JSON object' };
    }
    const envelope = parsed as Record<string, unknown>;
    // A missing `v` is accepted: the field is optional so that a minimal client
    // can talk to a hub without knowing the version.
    if (envelope.v !== undefined && envelope.v !== PANEL_VERSION) {
        return {
            kind: 'rejected',
            code: 'unsupported_version',
            detail: `this hub speaks panel protocol version ${PANEL_VERSION}`,
        };
    }
    if (!isPanelMessageType(envelope.type)) {
        return {
            kind: 'unknown_type',
            type: typeof envelope.type === 'string' ? envelope.type : String(envelope.type),
        };
    }
    // The body is the caller's to check; this claims only what was verified.
    return { kind: 'ok', message: envelope as unknown as PanelMessage };
}
