/** Commands accepted by the composer's explicit command mode. */
export interface ComposerCommand {
    readonly id: 'refresh-conversation' | 'continue-run';
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
];

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
): string | null {
    if (command.id !== 'continue-run') return null;
    if (!connected) return 'Connect a worker first.';
    if (runActive) return 'Wait for the current run to finish or cancel it.';
    return null;
}
