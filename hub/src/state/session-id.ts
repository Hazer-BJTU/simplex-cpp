/**
 * @file session identifier validation.
 *
 * The rule is core's own (`core::validate_session_id`): 1-128 ASCII letters,
 * digits, underscores, or hyphens. The hub validates before a session ID can
 * reach a filesystem path or a route, so a bad identifier is rejected with a
 * message instead of becoming a directory or an unmatched route.
 */

const SESSION_ID = /^[A-Za-z0-9_-]{1,128}$/;

/**
 * True when `id` may be used as a session identifier.
 *
 * The parameter is `unknown` rather than `string` because every caller is
 * checking a value that came from JSON, and a type predicate lets that check
 * narrow the value instead of being a boolean the caller has to re-derive.
 */
export function isValidSessionId(id: unknown): id is string {
    return typeof id === 'string' && SESSION_ID.test(id);
}

/** Throw a descriptive error unless `id` may be used as a session identifier. */
export function validateSessionId(id: unknown): string {
    if (!isValidSessionId(id)) {
        throw new Error(
            'session id must be 1-128 ASCII letters, digits, underscores or hyphens');
    }
    return id;
}

/** Reserved Hub-generated namespace; never accepted by manual session creation. */
export function isSubagentId(value: unknown): value is string {
    return typeof value === 'string'
        && /^subagent-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(value);
}
