/**
 * @file what the command palette offers, and what each entry means.
 *
 * Separated from the component for the usual reason: the interesting part is
 * which commands exist in which state — there is no "cancel the run" when no run
 * is active, no "switch to" a session the hub does not list — and that is a
 * pure function of the store's shape, so `node --test` can check it. The
 * component is left with rendering and with turning an action into a call.
 *
 * The actions are described rather than performed. A palette that closed over
 * the client would be testable only through the client, and the point of this
 * layer is that the *offer* is checked separately from the *doing*.
 */
import type { SessionId } from '../../../shared/protocol.ts';
import type { ConfirmMode, InspectorTab } from '../state/store.ts';
import type { SessionDescription } from '../../../shared/protocol.ts';

/** Everything the palette can be asked to do. */
export type PaletteAction =
    | { readonly kind: 'select-session'; readonly session: SessionId }
    | { readonly kind: 'worker'; readonly session: SessionId; readonly action: 'start' | 'stop' }
    | {
        readonly kind: 'signal';
        readonly session: SessionId;
        readonly operation: 'status' | 'options' | 'cancel';
    }
    | { readonly kind: 'reload-transcript'; readonly session: SessionId }
    | { readonly kind: 'inspector'; readonly open: boolean; readonly tab?: InspectorTab | undefined }
    | { readonly kind: 'toggle-details' }
    | { readonly kind: 'ping' };

/** One entry. */
export interface CommandSpec {
    readonly id: string;
    readonly group: 'session' | 'process' | 'view';
    readonly label: string;
    readonly hint?: string | undefined;
    readonly action: PaletteAction;
}

/** What the palette needs to know to decide what to offer. */
export interface PaletteInput {
    readonly sessions: readonly SessionDescription[];
    readonly selected: SessionId | null;
    /** True when the selected session's worker process is up. */
    readonly running: boolean;
    readonly runActive: boolean;
    readonly inspectorOpen: boolean;
    readonly showDetails: boolean;
    readonly confirmMode: ConfirmMode;
    readonly pingMs: number | null;
}

/** True when every character of `query` appears in `text`, in order. */
export function matches(text: string, query: string): boolean {
    if (!query) return true;
    const haystack = text.toLowerCase();
    let at = 0;
    for (const char of query.toLowerCase()) {
        const found = haystack.indexOf(char, at);
        if (found === -1) return false;
        at = found + 1;
    }
    return true;
}

/** The entries offered in one state, in the order they should be listed. */
export function buildCommands(input: PaletteInput): CommandSpec[] {
    const commands: CommandSpec[] = [];

    for (const session of input.sessions) {
        commands.push({
            id: `session:${session.session_id}`,
            group: 'session',
            label: `Switch to ${session.session_id}`,
            hint: session.session_id === input.selected ? 'current' : undefined,
            action: { kind: 'select-session', session: session.session_id },
        });
    }

    const selected = input.selected;
    if (selected) {
        commands.push({
            id: 'process:power',
            group: 'process',
            label: input.running ? 'Stop the worker' : 'Start the worker',
            hint: selected,
            action: { kind: 'worker', session: selected, action: input.running ? 'stop' : 'start' },
        });
        commands.push({
            id: 'process:status',
            group: 'process',
            label: 'Ask for a status snapshot',
            hint: selected,
            action: { kind: 'signal', session: selected, operation: 'status' },
        });
        commands.push({
            id: 'process:options',
            group: 'process',
            label: 'Ask which models and tools are available',
            hint: selected,
            action: { kind: 'signal', session: selected, operation: 'options' },
        });
        // Only offered when it can do something: a command that exists and then
        // refuses is worse than one that is not there.
        if (input.runActive) {
            commands.push({
                id: 'process:cancel',
                group: 'process',
                label: 'Cancel the active run',
                hint: selected,
                action: { kind: 'signal', session: selected, operation: 'cancel' },
            });
        }
        commands.push({
            id: 'transcript:reload',
            group: 'view',
            label: 'Reload this transcript from the hub',
            hint: selected,
            action: { kind: 'reload-transcript', session: selected },
        });
        for (const tab of ['run', 'process', 'logs', 'snapshot'] as InspectorTab[]) {
            commands.push({
                id: `inspector:${tab}`,
                group: 'view',
                label: `Open the ${tab} pane`,
                action: { kind: 'inspector', open: true, tab },
            });
        }
        commands.push({
            id: 'view:inspector',
            group: 'view',
            label: input.inspectorOpen ? 'Hide the context drawer' : 'Show the context drawer',
            action: { kind: 'inspector', open: !input.inspectorOpen },
        });
        commands.push({
            id: 'session:confirm-mode',
            group: 'view',
            label: 'Confirmation mode for this session',
            hint: input.confirmMode,
            action: { kind: 'inspector', open: true, tab: 'run' },
        });
    }

    commands.push({
        id: 'hub:ping',
        group: 'view',
        label: 'Ping the hub',
        hint: input.pingMs === null ? undefined : `${input.pingMs}ms`,
        action: { kind: 'ping' },
    });
    commands.push({
        id: 'view:details',
        group: 'view',
        label: input.showDetails ? 'Hide technical details' : 'Show technical details',
        action: { kind: 'toggle-details' },
    });

    return commands;
}

/** The entries whose label or hint matches a query. */
export function filterCommands(
    commands: readonly CommandSpec[],
    query: string,
): CommandSpec[] {
    return commands.filter(
        (command) => matches(`${command.label} ${command.hint ?? ''}`, query),
    );
}
