/** Exact UTF-8 lengths with multibyte text and JSON-escaped characters. */
export function compactSummary(bytes) {
    const fragment = 'Goal: finish the task. State: verified. References: 中文🌍\n"\\\t';
    const repeated = fragment.repeat(Math.floor(bytes / Buffer.byteLength(fragment)));
    return repeated + 'S'.repeat(bytes - Buffer.byteLength(repeated));
}
