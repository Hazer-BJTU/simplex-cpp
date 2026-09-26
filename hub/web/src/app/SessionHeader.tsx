/**
 * @file the session header, in three layers.
 *
 * The old header was ten peer buttons of the same size and weight, ordered as
 * they happened to be written: three kinds of action — process lifecycle,
 * control signals, and destructive operations — with no grouping, and a single
 * `disabled = !session` flag on all of them. So Stop was clickable with no
 * process, Delete was clickable while one was running, and `Force kill` was red
 * while `Delete`, the one that actually removes data, was a ghost.
 *
 * The layers here are the design's:
 *
 *   primary    Start ⇄ Stop, one solid button, the state decides which;
 *   common     Status, Options, approvals — icon buttons with tooltips;
 *   overflow   Restart, Shutdown, Force kill, Delete — a menu, with the
 *              destructive entries in red and behind a dialog.
 *
 * Every control is enabled exactly when it can do something, and says why when
 * it cannot. That is the whole of the fix for "button availability divorced
 * from real state": a disabled control here has a reason attached to it.
 */
import { useState } from 'react';
import type { SessionDescription } from '../../../shared/protocol.ts';
import { usePanel, useSession, useView } from '../state/usePanel.ts';
import { Badge, Button, IconButton } from '../ui/Button.tsx';
import { Glyph } from '../ui/icons.tsx';
import {
    Dialog,
    DialogButton,
    DialogContent,
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
                label: process.pid === null ? 'running' : `running · pid ${process.pid}`,
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
                    + ' with the same specification. Anything the worker has not persisted is lost.',
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
                        + ' **not** configured to kill the process group, so a process the worker'
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
                body: 'The session, its conversation snapshot, and its hub event history are'
                    + ' deleted. Files created by tools in the worker directory are kept.'
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
    const model = usePanel((state) => (state.selected ? state.model(state.selected) : ''));
    const [pending, setPending] = useState<Pending | null>(null);

    if (!selected || !session) return null;

    // Bound to a local so the narrowing survives into the callbacks below,
    // which the compiler cannot narrow through.
    const sessionId: string = selected;
    const state = processState(session);
    const runActive = Boolean(view?.runActive);
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
        setPending(describe(action, { processGroup }));
    }

    function run(action: Pending['action']): void {
        setPending(null);
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
                <Badge tone={state.tone}>{state.label}</Badge>
                {session.connected && <Badge tone="info">worker attached</Badge>}
                {runActive && <Badge tone="info">run active</Badge>}
                {pendingApprovals > 0 && (
                    <Badge tone="warn" title="tool calls waiting for a decision">
                        {pendingApprovals} approval{pendingApprovals === 1 ? '' : 's'}
                    </Badge>
                )}
                {model && <span className="text-xs text-ink-muted">{model}</span>}

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
                        title="launch a worker process for this session"
                    >
                        Start
                    </Button>
                )}

                <Tooltip label="Ask the worker for a status snapshot">
                    <IconButton
                        label="Status"
                        onClick={() => client.sendSignal(sessionId, 'status')}
                        disabled={!session.connected}
                    >
                        <Glyph name="status" />
                    </IconButton>
                </Tooltip>
                <Tooltip label="Ask the worker which models and tools it offers">
                    <IconButton
                        label="Options"
                        onClick={() => client.sendSignal(sessionId, 'options')}
                        disabled={!session.connected}
                    >
                        <Glyph name="options" />
                    </IconButton>
                </Tooltip>
                <Tooltip label={runActive ? 'Ask the worker to cancel the active run' : 'No run is active'}>
                    <IconButton
                        label="Cancel"
                        onClick={() => client.sendSignal(sessionId, 'cancel')}
                        disabled={!session.connected || !runActive}
                    >
                        <Glyph name="cancel" />
                    </IconButton>
                </Tooltip>
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
                        <IconButton label="More actions">
                            <Glyph name="more" />
                        </IconButton>
                    </MenuTrigger>
                    <MenuContent>
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
                <Dialog open onOpenChange={(open) => { if (!open) setPending(null); }}>
                    <DialogContent
                        title={pending.title}
                        description={`session ${sessionId}`}
                        footer={
                            <>
                                {/* Cancel is first in the DOM, which is where
                                    Radix puts focus — so Enter cancels rather
                                    than confirming a destructive action the
                                    operator has not read yet. */}
                                <DialogButton onClick={() => setPending(null)}>Cancel</DialogButton>
                                <DialogButton
                                    variant={pending.danger ? 'danger' : 'primary'}
                                    onClick={() => run(pending.action)}
                                >
                                    {pending.confirm}
                                </DialogButton>
                            </>
                        }
                    >
                        <p className="whitespace-pre-wrap">{pending.body}</p>
                    </DialogContent>
                </Dialog>
            )}
        </div>
    );
}
