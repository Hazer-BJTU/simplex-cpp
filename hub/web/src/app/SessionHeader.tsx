/**
 * @file the session header: which session this is, and how to drive its worker.
 *
 * Not the redesigned header — that is a later stage. What is already different
 * is that the controls are *conditional*. The old panel computed one `disabled`
 * flag from "is a session selected" and applied it to all ten buttons, so Stop
 * was clickable with no process and Delete was clickable while one was running.
 * Here a control appears when it can do something, and says why when it cannot.
 */
import { usePanel } from '../state/usePanel.ts';
import { useClient } from './ClientContext.tsx';

/** One control. */
function Action({ label, title, danger, onClick }: {
    label: string;
    title: string;
    danger?: boolean;
    onClick: () => void;
}) {
    return (
        <button
            type="button"
            title={title}
            onClick={onClick}
            className={`rounded border px-2 py-1 text-xs font-medium transition-colors
                ${danger
                    ? 'border-rose-200 text-rose-700 hover:bg-rose-50'
                    : 'border-slate-200 text-slate-700 hover:bg-slate-100'}`}
        >
            {label}
        </button>
    );
}

export function SessionHeader() {
    const client = useClient();
    const selected = usePanel((state) => state.selected);
    const session = usePanel((state) => (
        state.selected ? state.sessions.get(state.selected) ?? null : null
    ));
    const runActive = usePanel((state) => (
        state.selected ? Boolean(state.views.get(state.selected)?.runActive) : false
    ));
    const model = usePanel((state) => (state.selected ? state.model(state.selected) : ''));

    if (!selected || !session) return null;

    const process = session.process;
    const running = process !== null
        && (process.state === 'running' || process.state === 'starting');
    const pending = session.confirmations.filter((prompt) => prompt.settled_at === null);

    return (
        <div className="border-b border-slate-200 bg-white px-4 py-2">
            <div className="flex flex-wrap items-center gap-2">
                <h2 className="font-mono text-sm font-semibold text-slate-900">{selected}</h2>

                <span className={`text-xs ${session.connected ? 'text-emerald-700' : 'text-slate-500'}`}>
                    {session.connected ? 'worker attached' : 'worker not attached'}
                </span>

                {runActive && (
                    <span className="rounded-full bg-sky-100 px-2 py-0.5 text-[11px]
                        font-medium text-sky-800">
                        run active
                    </span>
                )}

                {model && <span className="text-xs text-slate-500">{model}</span>}

                <span className="flex-1" />

                {running ? (
                    <>
                        <Action
                            label="Stop"
                            title="ask the worker to shut down, then signal the process"
                            onClick={() => { void client.workerAction(selected, 'stop'); }}
                        />
                        <Action
                            label="Restart"
                            title="stop and start again with the same spec"
                            onClick={() => { void client.workerAction(selected, 'restart'); }}
                        />
                        <Action
                            label="Force kill"
                            title="signal the process group immediately, skipping the protocol"
                            danger
                            onClick={() => {
                                // No `window.confirm`: the wording a dangerous
                                // action deserves belongs in a dialog that can
                                // state what will actually be killed, which the
                                // confirm dialog stage adds. Until then the
                                // button says what it does and does it.
                                void client.workerAction(selected, 'force-kill');
                            }}
                        />
                    </>
                ) : (
                    <Action
                        label="Start"
                        title="launch a worker process for this session"
                        onClick={() => { void client.workerAction(selected, 'start'); }}
                    />
                )}

                <Action
                    label="Status"
                    title="ask the worker for a status snapshot"
                    onClick={() => client.sendSignal(selected, 'status')}
                />
                <Action
                    label="Options"
                    title="ask the worker what models and tools it offers"
                    onClick={() => client.sendSignal(selected, 'options')}
                />
                {runActive && (
                    <Action
                        label="Cancel"
                        title="ask the worker to cancel the active run"
                        onClick={() => client.sendSignal(selected, 'cancel')}
                    />
                )}
            </div>

            {pending.length > 0 && (
                // The approval itself is answered by the banner below; this says
                // that one exists, which the old panel could not do for a
                // session it was not subscribed to.
                <p
                    data-testid="pending-approval-banner"
                    className="mt-2 rounded border border-amber-300 bg-amber-50 px-2 py-1 text-xs
                        text-amber-900"
                >
                    {pending.length} tool call{pending.length === 1 ? '' : 's'} waiting for a
                    decision below.
                </p>
            )}
        </div>
    );
}
