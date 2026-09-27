/** Commands accepted by the composer's explicit command mode. */
export interface ComposerCommand {
    readonly id: 'refresh-conversation' | 'continue-run' | 'compact-context';
    readonly name: string;
    readonly detail: string;
}

export const COMPOSER_COMMANDS: readonly ComposerCommand[] = [
    {
        id: 'refresh-conversation',
        name: 'Refresh conversation',
        detail: 'Recover hub events and the connected worker’s conversation history.',
    },
    {
        id: 'continue-run',
        name: 'Continue run',
        detail: 'Resume from the worker’s current state without sending a new message.',
    },
    {
        id: 'compact-context',
        name: 'Compact context',
        detail: 'Archive the conversation and replace its context with a saved summary.',
    },
];

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

/** Match from the beginning of a command name; no slash syntax is reserved. */
export function matchingComposerCommands(query: string): readonly ComposerCommand[] {
    const prefix = query.trimStart().toLocaleLowerCase();
    return COMPOSER_COMMANDS.filter((command) => (
        command.name.toLocaleLowerCase().startsWith(prefix)
    ));
}

/** A missing reason means the command can be submitted. */
export function unavailableReason(
    command: ComposerCommand,
    connected: boolean,
    runActive: boolean,
    compactSupported = false,
): string | null {
    if (command.id === 'refresh-conversation') return null;
    if (!connected) return 'Connect a worker first.';
    if (runActive) return 'Wait for the current run to finish or cancel it.';
    if (command.id === 'compact-context' && !compactSupported) {
        return 'This hub or worker does not support context compaction.';
    }
    return null;
}
