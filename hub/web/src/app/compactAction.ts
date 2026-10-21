/** Show the actual worker policy, including older workers that do not report it. */
export function compactRetentionDetail(value: unknown): string {
    if (!value || typeof value !== 'object') return 'Archive retention is not reported by this worker.';
    const policy = value as Record<string, unknown>;
    const maxArchives = policy.max_archives;
    if (typeof maxArchives !== 'number'
        || !Number.isSafeInteger(maxArchives) || maxArchives < 0) {
        return 'Archive retention is not reported by this worker.';
    }
    return maxArchives > 0
        ? `Automatic cleanup after success: ${maxArchives} ${maxArchives === 1 ? 'archive' : 'archives'}. The current archive is always kept.`
        : 'Automatic archive cleanup is disabled; archives accumulate on disk.';
}

