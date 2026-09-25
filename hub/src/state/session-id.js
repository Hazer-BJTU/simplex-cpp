/**
 * @file session identifier validation.
 *
 * The rule is core's own (`core::validate_session_id`): 1-128 ASCII letters,
 * digits, underscores, or hyphens. The hub validates before a session ID can
 * reach a filesystem path or a route, so a bad identifier is rejected with a
 * message instead of becoming a directory or an unmatched route.
 */

const SESSION_ID = /^[A-Za-z0-9_-]{1,128}$/;

/** True when `id` may be used as a session identifier. */
export function isValidSessionId(id) {
    return typeof id === 'string' && SESSION_ID.test(id);
}

/** Throw a descriptive error unless `id` may be used as a session identifier. */
export function validateSessionId(id) {
    if (typeof id !== 'string' || !SESSION_ID.test(id)) {
        throw new Error(
            'session id must be 1-128 ASCII letters, digits, underscores or hyphens');
    }
    return id;
}
