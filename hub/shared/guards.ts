/**
 * @file runtime checks for the panel protocol envelope.
 *
 * The types in `protocol.ts` describe what a message looks like; nothing at
 * runtime enforces them, because the wire is JSON. This module is that
 * enforcement, kept deliberately narrow: it validates the *envelope* — that the
 * text parses, that it is an object, that `v` is the version both ends speak,
 * that `type` is one the protocol defines.
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
 *
 * Both directions are checked here rather than one per end. The hub used to
 * validate what it received and the panel used to trust whatever arrived, which
 * meant the browser's front door was the one place an unfamiliar or malformed
 * frame could reach application code. `checkHubEnvelope` closes that, and it
 * runs under `node --test` because it lives here instead of in the bundle.
 */
import {
    HUB_MESSAGE_TYPES,
    PANEL_MESSAGE_TYPES,
    PANEL_VERSION,
    type ErrorCode,
    type HubMessage,
    type HubMessageType,
    type PanelMessage,
    type PanelMessageType,
} from './protocol.ts';

/**
 * Outcome of checking one inbound envelope.
 *
 * `unknown_type` is separated from `rejected` on purpose: the first is a
 * message from a *newer* peer and must be ignored, the second is a message that
 * cannot be understood at all.
 */
export interface EnvelopeVerdict<T> {
    /** A known message type with a valid envelope. The body is still unchecked. */
    kind: 'ok';
    message: T;
}

/** Outcome of checking one inbound panel message. */
export type EnvelopeCheck =
    | EnvelopeVerdict<PanelMessage>
    /** The type is not one this protocol version defines: ignore it. */
    | { kind: 'unknown_type'; type: string }
    /** The envelope itself is unusable; answer with `code`. */
    | { kind: 'rejected'; code: ErrorCode; detail: string };

/** Outcome of checking one inbound hub message. */
export type HubEnvelopeCheck =
    | EnvelopeVerdict<HubMessage>
    | { kind: 'unknown_type'; type: string }
    | { kind: 'rejected'; code: ErrorCode; detail: string };

/** True when `type` is a message type this protocol version defines. */
export function isPanelMessageType(type: unknown): type is PanelMessageType {
    return typeof type === 'string'
        && (PANEL_MESSAGE_TYPES as readonly string[]).includes(type);
}

/** True when `type` is a hub message type this protocol version defines. */
export function isHubMessageType(type: unknown): type is HubMessageType {
    return typeof type === 'string'
        && (HUB_MESSAGE_TYPES as readonly string[]).includes(type);
}

/** The shared half of both checks: parse, then validate the envelope fields. */
function checkShape<T>(
    text: string,
    isKnownType: (type: unknown) => type is T,
    describeVersion: (received: unknown) => string,
): EnvelopeVerdict<never> | { kind: 'unknown_type'; type: string }
    | { kind: 'rejected'; code: ErrorCode; detail: string } {
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
            detail: describeVersion(envelope.v),
        };
    }
    if (!isKnownType(envelope.type)) {
        return {
            kind: 'unknown_type',
            type: typeof envelope.type === 'string' ? envelope.type : String(envelope.type),
        };
    }
    // The body is the caller's to check; this claims only what was verified.
    return { kind: 'ok', message: envelope as never };
}

/**
 * Parse and check one inbound panel message.
 *
 * @param text the frame as text. The caller decodes bytes; a binary frame is
 *   refused before it gets here, with its own error code.
 */
export function checkEnvelope(text: string): EnvelopeCheck {
    return checkShape(
        text,
        isPanelMessageType,
        // Names this build's version, which is what the hub answers with.
        () => `this hub speaks panel protocol version ${PANEL_VERSION}`,
    ) as EnvelopeCheck;
}

/**
 * Parse and check one inbound hub message, from the panel's side.
 *
 * The same shape rules, reported from the other end: a version mismatch names
 * both numbers, because the panel's useful next move is to tell the operator
 * that the hub is newer than the page they are looking at.
 */
export function checkHubEnvelope(text: string): HubEnvelopeCheck {
    return checkShape(
        text,
        isHubMessageType,
        (received) => `this hub speaks panel protocol version ${received},`
            + ` this panel speaks ${PANEL_VERSION}`,
    ) as HubEnvelopeCheck;
}
