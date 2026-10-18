/** The successful, durably published context replacement reported by a worker. */
export interface CompactResult {
    readonly origin?: 'automatic';
    readonly summary: string;
    readonly memory_file?: string;
    /** Storage evidence, not a downloadable file or an archive-query endpoint. */
    readonly archive?: {
        readonly session_id: string;
        readonly archive_id: string;
        readonly storage_kind: 'json' | 'sqlite';
        readonly restorable: true;
    };
    readonly removed_turns: number;
    readonly revision: number;
    readonly durable: true;
    readonly archive_cleanup_error?: string;
}

/** Invalid or failed results must never invalidate the displayed conversation. */
export function parseCompactResult(value: unknown): CompactResult | null {
    if (!value || typeof value !== 'object' || Array.isArray(value)) return null;
    const data = value as Record<string, unknown>;
    const candidate = data.archive;
    const archive = candidate && typeof candidate === 'object' && !Array.isArray(candidate)
        ? candidate as Record<string, unknown> : null;
    const validArchive: CompactResult['archive'] = archive
        && typeof archive.session_id === 'string' && archive.session_id
        && typeof archive.archive_id === 'string' && archive.archive_id
        && (archive.storage_kind === 'json' || archive.storage_kind === 'sqlite')
        && archive.restorable === true
        ? { session_id: archive.session_id, archive_id: archive.archive_id,
            storage_kind: archive.storage_kind, restorable: true }
        : undefined;
    const file = typeof data.memory_file === 'string' && data.memory_file ? data.memory_file : undefined;
    if (typeof data.summary !== 'string' || !data.summary.trim()
        || (!file && !validArchive)
        || typeof data.removed_turns !== 'number' || !Number.isSafeInteger(data.removed_turns)
        || data.removed_turns < 0 || typeof data.revision !== 'number'
        || !Number.isSafeInteger(data.revision) || data.revision < 0
        || data.durable !== true) return null;
    return {
        ...(data.origin === 'automatic' ? { origin: 'automatic' as const } : {}),
        summary: data.summary,
        ...(file ? { memory_file: file } : {}),
        ...(validArchive ? { archive: validArchive } : {}),
        removed_turns: data.removed_turns,
        revision: data.revision,
        durable: true,
        ...(typeof data.archive_cleanup_error === 'string'
            ? { archive_cleanup_error: data.archive_cleanup_error } : {}),
    };
}
