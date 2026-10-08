/** Display-only batch omissions are not executable calls or returned results. */
export function toolOmissionCount(value: unknown): number {
    if (value === null || typeof value !== 'object' || Array.isArray(value)) return 0;
    const marker = value as Record<string, unknown>;
    if (marker.display_omitted !== true
        || ['id', 'name', 'query', 'invoke_return'].some(key => key in marker)) return 0;
    // A legacy whole-entry omission without a valid aggregate count represents
    // one entry. Positive safe counts describe the entire omitted suffix.
    return typeof marker.omitted_items === 'number'
        && Number.isSafeInteger(marker.omitted_items) && marker.omitted_items > 0
        ? marker.omitted_items : 1;
}

/** Saturating accounting keeps repeated normalization in the JSON safe-integer domain. */
export function omittedToolItems(value: unknown): number {
    if (!Array.isArray(value)) return 0;
    let count = 0;
    for (const item of value) {
        count = Math.min(Number.MAX_SAFE_INTEGER, count + toolOmissionCount(item));
    }
    return count;
}
