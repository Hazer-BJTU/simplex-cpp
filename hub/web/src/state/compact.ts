/** The successful, durably published context replacement reported by a worker. */
export interface CompactResult {
    readonly origin?: 'automatic';
    readonly summary: string;
    readonly memory_file: string;
    readonly removed_turns: number;
    readonly revision: number;
    readonly durable: true;
    readonly archive_cleanup_error?: string;
}

/** Invalid or failed results must never invalidate the displayed conversation. */
export function parseCompactResult(value: unknown): CompactResult | null {
    if (!value || typeof value !== 'object' || Array.isArray(value)) return null;
    const data = value as Record<string, unknown>;
    if (typeof data.summary !== 'string' || !data.summary.trim()
        || typeof data.memory_file !== 'string' || !data.memory_file
        || typeof data.removed_turns !== 'number' || !Number.isSafeInteger(data.removed_turns)
        || data.removed_turns < 0 || typeof data.revision !== 'number'
        || !Number.isSafeInteger(data.revision) || data.revision < 0
        || data.durable !== true) return null;
    return {
        ...(data.origin === 'automatic' ? { origin: 'automatic' as const } : {}),
        summary: data.summary,
        memory_file: data.memory_file,
        removed_turns: data.removed_turns,
        revision: data.revision,
        durable: true,
        ...(typeof data.archive_cleanup_error === 'string'
            ? { archive_cleanup_error: data.archive_cleanup_error } : {}),
    };
}
