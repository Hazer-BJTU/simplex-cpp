/** Session identity, worker lifecycle controls, and the independent Inspector.
 * Request activity and cancellation belong to the conversation and composer.
 */
import { useEffect, useRef, useState } from 'react';
import type { SessionDescription } from '../../../ui/index.ts';
import { usePanel, useSession, useView } from '../state/usePanel.ts';
import { Badge, Button, IconButton } from '../ui/Button.tsx';
import { Glyph } from '../ui/icons.tsx';
import { Status } from '../ui/Status.tsx';
import {
    Menu,
    MenuContent,
    MenuItem,
    MenuLabel,
    MenuSeparator,
    MenuTrigger,
    Tooltip,
} from '../ui/overlays.tsx';
import { useClient } from './ClientContext.tsx';

/** What the worker process is doing, in the words the buttons use. */
function processState(session: SessionDescription): {
    running: boolean;
    label: string;
    tone: 'neutral' | 'info' | 'ok' | 'warn' | 'bad';
} {
    const process = session.process;
    if (!process) return { running: false, label: 'no process', tone: 'neutral' };
    switch (process.state) {
        case 'running':
            return {
                running: true,
                label: 'running',
                tone: 'ok',
            };
        case 'starting':
            return { running: true, label: 'starting', tone: 'info' };
        case 'stopping':
            return { running: true, label: 'stopping', tone: 'warn' };
        case 'exited':
            return {
                running: false,
                label: process.exit_code === null ? 'exited' : `exited (${process.exit_code})`,
                tone: 'neutral',
            };
        case 'failed':
            return {
                running: false,
                label: process.error ? `failed: ${process.error}` : 'failed',
                tone: 'bad',
            };
        default:
            return { running: false, label: process.state, tone: 'neutral' };
    }
}

/** One destructive action awaiting confirmation. */
interface Pending {
    action: 'restart' | 'force-kill' | 'shutdown' | 'delete';
    title: string;
    body: string;
    confirm: string;
    danger: boolean;
}

/**
 * What an action will actually do.
 *
 * The old panel's force-kill confirmation claimed it would kill the process
 * group unconditionally, while the hub only does that when it is configured to
 * (defect D27). A confirmation that describes the wrong consequence is worse
 * than none: it teaches the operator to stop reading them.
 */
function describe(action: Pending['action'], options: { processGroup: boolean }): Pending {
    switch (action) {
        case 'restart':
            return {
                action,
                title: 'Restart this worker?',
                body: 'The current worker is asked to shut down and a new one is launched'
                    + ' using its saved configuration with refreshed hub connections. Unsaved work is lost.',
                confirm: 'Restart',
                danger: false,
            };
        case 'force-kill':
            return {
                action,
                title: 'Force kill this worker?',
                body: options.processGroup
                    ? 'The worker is signalled immediately, skipping the protocol. This hub is'
                        + ' configured to kill the whole process group, so any process the worker'
                        + ' started is killed with it.'
                    : 'The worker is signalled immediately, skipping the protocol. This hub is'
                        + ' not configured to kill the process group, so a process the worker'
                        + ' started may outlive it.',
                confirm: 'Force kill',
                danger: true,
            };
        case 'shutdown':
            return {
                action,
                title: 'Ask the worker to shut down?',
                body: 'The worker is asked to stop over the protocol and is given time to finish.'
                    + ' This is the same as Stop; it is here for when the worker is attached but'
                    + ' the panel is not driving it.',
                confirm: 'Shut down',
                danger: false,
            };
        case 'delete':
            return {
                action,
                title: 'Delete this session?',
                body: 'The entire session directory is deleted, including its configuration,'
                    + ' snapshots, memory archives, logs, and any tool-created files inside it.'
                    + ' The managed allocation on its target is retired as well.'
                    + ' This cannot be undone.',
                confirm: 'Delete',
                danger: true,
            };
    }
}

export function SessionHeader() {
    const client = useClient();
    const selected = usePanel((state) => state.selected);
    const session = useSession(selected);
    const view = useView(selected);
    const hub = usePanel((state) => state.hub);
    const inspectorOpen = usePanel((state) => state.inspectorOpen);
    const setInspectorOpen = usePanel((state) => state.setInspectorOpen);
    const [pending, setPending] = useState<Pending | null>(null);
    const cancelRef = useRef<HTMLButtonElement>(null);
    // What the open menu asked for, readable from the menu's close handler: the
    // state update has not rendered there yet, and the ref is the only way to
    // know whether the close is a confirmation or a plain dismissal.
    const asked = useRef<Pending | null>(null);
    const moreRef = useRef<HTMLButtonElement>(null);
    const confirmRef = useRef<HTMLDivElement>(null);
    // Closing without deciding is always available and never remembers state.
    function dismiss(): void {
        asked.current = null;
        // Focus is on Cancel while the question is open; closing it must not
        // drop a keyboard user at the top of the document.
        if (confirmRef.current?.contains(document.activeElement)) moreRef.current?.focus();
        setPending(null);
    }
    useEffect(() => {
        if (!pending) return;
        function onKeyDown(event: KeyboardEvent): void {
            if (event.key !== 'Escape') return;
            dismiss();
            // The panel's drawers listen on document too; this one is answered.
            event.stopPropagation();
        }
        document.addEventListener('keydown', onKeyDown);
        return () => document.removeEventListener('keydown', onKeyDown);
    }, [pending]);
    // A confirmation belongs to the session that asked for it. Switching
    // sessions drops it rather than leaving another session's process on offer.
    useEffect(() => {
        asked.current = null;
        setPending(null);
    }, [selected]);

    if (!selected || !session) return null;

    // Bound to a local so the narrowing survives into the callbacks below,
    // which the compiler cannot narrow through.
    const sessionId: string = selected;
    const state = processState(session);
    const processGroup = hub?.force_kill_process_group ?? false;
    const pendingApprovals = [...(view?.confirmations.values() ?? [])]
        .filter((prompt) => prompt.settled_at === null).length;

    // The hub refuses to delete a session whose worker is running or attached,
    // so the menu says which of the two it is rather than just greying out.
    const busy = state.running || session.connected;
    const deleteHint = state.running
        ? 'stop the worker first'
        : session.connected ? 'the worker is still attached' : undefined;

    function ask(action: Pending['action']): void {
        const next = describe(action, { processGroup });
        asked.current = next;
        setPending(next);
    }

    function run(action: Pending['action']): void {
        dismiss();
        if (action === 'delete') {
            void client.deleteSession(sessionId);
            return;
        }
        if (action === 'shutdown') {
            client.sendSignal(sessionId, 'shutdown');
            return;
        }
        void client.workerAction(sessionId, action);
    }

    return (
        <div className="animate-enter border-b border-line bg-surface px-3 py-2 sm:px-4">
            <div className="flex flex-wrap items-center gap-x-2 gap-y-1">
                <h2 data-testid="session-title" className="font-mono text-sm font-semibold text-ink">
                    {sessionId}
                </h2>
                {(!session.connected || (session.process && session.process.state !== 'running')) && (
                    <Badge tone={state.tone}>{state.label}</Badge>
                )}
                <Status tone={session.connected ? 'info' : 'warn'}>
                    {session.connected ? 'worker attached' : 'worker disconnected'}
                </Status>
                {pendingApprovals > 0 && (
                    <Badge tone="warn" title="tool calls waiting for a decision">
                        {pendingApprovals} approval{pendingApprovals === 1 ? '' : 's'}
                    </Badge>
                )}

                <span className="flex-1" />

                {/* The primary action: one button, and the state picks which. */}
                {state.running ? (
                    <Button
                        variant="primary"
                        data-testid="session-primary-action"
                        icon={<Glyph name="stop" size="sm" />}
                        onClick={() => { void client.workerAction(sessionId, 'stop'); }}
                        title="ask the worker to shut down, then signal the process"
                    >
                        Stop
                    </Button>
                ) : (
                    <Button
                        variant="primary"
                        data-testid="session-primary-action"
                        icon={<Glyph name="start" size="sm" />}
                        onClick={() => { void client.workerAction(sessionId, 'start'); }}
                        title="start using the saved configuration if present; refresh hub connections"
                    >
                        Start
                    </Button>
                )}

                <Tooltip label={inspectorOpen ? 'Hide the context drawer' : 'Show the context drawer'}>
                    <IconButton
                        label={inspectorOpen ? 'Hide inspector' : 'Show inspector'}
                        onClick={() => setInspectorOpen(!inspectorOpen)}
                    >
                        <Glyph name="inspector" />
                    </IconButton>
                </Tooltip>

                <Menu>
                    <MenuTrigger asChild>
                        <IconButton ref={moreRef} label="More actions">
                            <Glyph name="more" />
                        </IconButton>
                    </MenuTrigger>
                    <MenuContent onCloseAutoFocus={(event) => {
                        // A menu item that asks a question hands focus to that
                        // question's safe answer instead of back to the trigger,
                        // so Enter on the trigger cannot re-open the menu over a
                        // confirmation the operator has not read.
                        const next = asked.current;
                        if (!next) return;
                        event.preventDefault();
                        cancelRef.current?.focus();
                    }}>
                        <MenuLabel>process</MenuLabel>
                        <MenuItem
                            disabled={!state.running}
                            hint={state.running ? undefined : 'no process is running'}
                            onSelect={() => ask('restart')}
                        >
                            <span className="inline-flex items-center gap-2">
                                <Glyph name="restart" size="sm" />
                                Restart
                            </span>
                        </MenuItem>
                        <MenuItem
                            disabled={!session.connected}
                            hint={session.connected ? undefined : 'no worker is attached'}
                            onSelect={() => ask('shutdown')}
                        >
                            <span className="inline-flex items-center gap-2">
                                <Glyph name="shutdown" size="sm" />
                                Shut down over the protocol
                            </span>
                        </MenuItem>

                        <MenuSeparator />
                        <MenuLabel>destructive</MenuLabel>
                        <MenuItem
                            danger
                            disabled={!state.running}
                            hint={state.running ? undefined : 'no process is running'}
                            onSelect={() => ask('force-kill')}
                        >
                            <span className="inline-flex items-center gap-2">
                                <Glyph name="force-kill" size="sm" />
                                Force kill
                            </span>
                        </MenuItem>
                        <MenuItem
                            danger
                            disabled={busy}
                            hint={deleteHint}
                            onSelect={() => ask('delete')}
                        >
                            <span className="inline-flex items-center gap-2">
                                <Glyph name="delete" size="sm" />
                                Delete session
                            </span>
                        </MenuItem>
                    </MenuContent>
                </Menu>
            </div>

            {pending && (
                // The question is answered in place, where it was asked: the
                // header is already the operator's, and a scrim over the
                // conversation hides the very context the description talks
                // about. Escape and the focused Cancel are the way out.
                <div
                    ref={confirmRef}
                    role="group"
                    aria-label={pending.title}
                    data-testid="session-confirm"
                    className="mt-2 animate-enter"
                >
                    <div className="reading-width border-t border-line pt-2">
                        <div className="flex flex-wrap items-baseline gap-x-3 gap-y-1">
                            <p className="max-w-prose text-xs text-ink">
                                <span className="font-medium">{pending.title}</span>{' '}
                                <span id="session-confirm-body" className="text-ink-muted">
                                    {pending.body}
                                </span>
                            </p>
                            <span className="ml-auto flex items-center gap-3">
                                <button
                                    ref={cancelRef}
                                    type="button"
                                    aria-describedby="session-confirm-body"
                                    onClick={() => dismiss()}
                                    className="px-1 py-1 text-sm text-ink-muted underline-offset-2
                                        hover:underline"
                                >
                                    Cancel
                                </button>
                                <button
                                    type="button"
                                    onClick={() => run(pending.action)}
                                    className={`px-1 py-1 text-sm font-medium underline-offset-2
                                        hover:underline ${pending.danger ? 'text-danger' : 'text-ink'}`}
                                >
                                    {pending.confirm}
                                </button>
                            </span>
                        </div>
                    </div>
                </div>
            )}
        </div>
    );
}
