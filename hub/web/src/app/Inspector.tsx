/**
 * @file the context drawer.
 *
 * The old inspector was a permanent 320px column whose tabs accumulated: a pane
 * was marked hidden with an attribute, but its author stylesheet said
 * `display: flex`, and an author rule beats the browser's `[hidden]` rule — so
 * every tab ever visited stayed on the page at full height (defect A3). That
 * defect is not fixed here, it is made impossible: a tab that is not selected
 * is not rendered at all.
 *
 * The second thing this fixes is the snapshot race (D25). The old pane awaited
 * a fetch and then painted whatever came back, so switching sessions mid-request
 * put one session's state under another's header. A snapshot here carries the
 * session it belongs to, and a reply for a session the operator has left is
 * dropped by the store.
 *
 * What it shows is deliberately plain: values the worker reported, not a
 * re-description of them. Where the old panel dumped 21 rows of `dl.kv`, this
 * groups the same facts so that "is it running" and "why did it stop" are
 * answerable at a glance.
 */
import { RefreshCw } from 'lucide-react';
import { useMemo, useState } from 'react';
import type { ProcessDescription, SessionDescription } from '../../../shared/protocol.ts';
import { usePanel, useSession, useView } from '../state/usePanel.ts';
import { statsOf } from '../state/view.ts';
import { Badge, Button, IconButton } from '../ui/Button.tsx';
import { Tabs, TabsList, TabsPanel, TabsTrigger, Tooltip } from '../ui/overlays.tsx';
import type { InspectorTab } from '../state/store.ts';
import { prettyJson } from './content.ts';
import { useClient } from './ClientContext.tsx';

/** One key/value row. */
function Row({ label, children }: { label: string; children: React.ReactNode }) {
    return (
        <div className="flex gap-2 py-0.5 text-[11px]">
            <span className="w-32 shrink-0 text-slate-400">{label}</span>
            <span className="min-w-0 flex-1 break-words font-mono text-slate-700">{children}</span>
        </div>
    );
}

/** A section heading. */
function Section({ title, children }: { title: string; children: React.ReactNode }) {
    return (
        <section className="mb-3">
            <h3 className="mb-1 text-[10px] font-semibold uppercase tracking-wide text-slate-400">
                {title}
            </h3>
            {children}
        </section>
    );
}

/** What the run is doing, from the worker's own status snapshot. */
function RunPane({ sessionId }: { sessionId: string }) {
    const status = usePanel((state) => state.statusData(sessionId)) as
        Record<string, unknown> | null;
    const identity = usePanel((state) => (
        state.sessions.get(sessionId)?.identity ?? null
    ));
    const loop = usePanel((state) => state.loop(sessionId));
    const model = usePanel((state) => state.model(sessionId));
    const view = useView(sessionId);
    const stats = useMemo(() => statsOf(view), [view]);
    const [showRaw, setShowRaw] = useState(false);

    if (!status) {
        return (
            <p className="p-2 text-[11px] text-slate-500">
                No status snapshot yet. The worker sends one when it starts and whenever
                Status is asked for.
            </p>
        );
    }

    const active = status.active === true;
    const pending = Array.isArray(loop?.pending_results) ? loop.pending_results.length : 0;

    return (
        <div className="p-2">
            <Section title="run">
                <Row label="active">{active ? 'yes' : 'no'}</Row>
                <Row label="stopping">{status.stopping === true ? 'yes' : 'no'}</Row>
                {loop && <Row label="loop status">{String(loop.status ?? '')}</Row>}
                {loop && <Row label="phase">{String(loop.phase ?? '')}</Row>}
                {loop && (
                    <Row label="completed exchanges">{String(loop.completed_exchanges ?? '')}</Row>
                )}
                {pending > 0 && <Row label="pending results">{pending}</Row>}
                {model && <Row label="model (session)">{model}</Row>}
            </Section>

            <Section title="storage">
                <Row label="storage failed">
                    {status.storage_failed === true
                        ? <span className="text-rose-700">yes — further saves are suppressed</span>
                        : 'no'}
                </Row>
                <Row label="rejected payloads">{String(status.rejected_payloads ?? 0)}</Row>
                {loop && loop.error !== '' && loop.error !== undefined && (
                    <Row label="loop error">
                        <span className="text-rose-700">{String(loop.error)}</span>
                    </Row>
                )}
            </Section>

            <Section title="worker identity">
                <Row label="state">{identity?.state ?? 'unknown'}</Row>
                <Row label="worker id">{identity?.worker_id ?? '(none seen)'}</Row>
                <Row label="since">{identity?.since ?? '—'}</Row>
            </Section>

            <Section title="this panel">
                <Row label="transcript items">{stats.items}</Row>
                <Row label="replay cursor">{stats.lastSeq}</Row>
                <Row label="gaps">{stats.gaps}</Row>
                <Row label="duplicates">{stats.duplicates}</Row>
                <Row label="dropped (bounded)">{stats.droppedItems}</Row>
                <Row label="unanswered requests">{stats.unknownRequests}</Row>
            </Section>

            <button
                type="button"
                className="text-[11px] text-slate-400 hover:text-slate-700"
                onClick={() => setShowRaw((value) => !value)}
            >
                {showRaw ? 'hide raw status' : 'raw status'}
            </button>
            {showRaw && (
                <pre className="mt-1 max-h-72 overflow-auto whitespace-pre-wrap break-all rounded
                    bg-slate-50 p-2 font-mono text-[11px] text-slate-700">
                    {prettyJson(status)}
                </pre>
            )}
        </div>
    );
}

/** The supervised process. */
function ProcessPane({ session }: { session: SessionDescription }) {
    const process: ProcessDescription | null = session.process;
    if (!process) {
        return (
            <p className="p-2 text-[11px] text-slate-500">
                No process has been started for this session by this hub. A worker started by a
                previous hub run is adopted, not restarted, so it would appear here.
            </p>
        );
    }
    const exited = process.exited_at !== null;
    return (
        <div className="p-2">
            <Section title="state">
                <Row label="state">
                    <Badge tone={process.state === 'running' ? 'ok' : process.state === 'failed' ? 'bad' : 'neutral'}>
                        {process.state}
                    </Badge>
                </Row>
                <Row label="pid">{process.pid ?? '—'}</Row>
                <Row label="started">{process.started_at}</Row>
                {exited && <Row label="exited">{process.exited_at}</Row>}
                {exited && <Row label="exit code">{process.exit_code ?? '—'}</Row>}
                {process.signal && <Row label="signal">{process.signal}</Row>}
                {process.error && (
                    <Row label="error"><span className="text-rose-700">{process.error}</span></Row>
                )}
                <Row label="stop requested">{process.stop_requested ? 'yes' : 'no'}</Row>
                <Row label="process group killed">
                    {process.process_group_killed ? 'yes' : 'no'}
                </Row>
            </Section>

            <Section title="invocation">
                <Row label="command">{process.command}</Row>
                <Row label="arguments">
                    {process.args.length === 0 ? '(none)' : process.args.join(' ')}
                </Row>
                <Row label="working directory">{process.cwd}</Row>
            </Section>

            <Section title="captured output">
                <Row label="log file">{process.log_path ?? '(not captured)'}</Row>
                <Row label="lines held">{process.log_lines}</Row>
                <Row label="lines dropped">{process.log_dropped}</Row>
            </Section>
        </div>
    );
}

/** The captured worker output. */
function LogsPane({ sessionId }: { sessionId: string }) {
    const client = useClient();
    const logs = useView(sessionId)?.logs ?? { lines: [], dropped: 0, logPath: null };
    return (
        <div className="flex h-full min-h-0 flex-col">
            <div className="flex shrink-0 items-center gap-2 px-2 py-1 text-[11px] text-slate-500">
                <span>{logs.lines.length} line(s)</span>
                {logs.dropped > 0 && <span className="text-amber-700">{logs.dropped} dropped</span>}
                {logs.logPath && (
                    <span className="truncate font-mono text-slate-400">{logs.logPath}</span>
                )}
                <span className="flex-1" />
                <Tooltip label="Ask the hub for the current tail">
                    <IconButton
                        label="Refresh logs"
                        onClick={() => client.refreshLogs(sessionId)}
                    >
                        <RefreshCw aria-hidden className="h-3.5 w-3.5" />
                    </IconButton>
                </Tooltip>
            </div>
            <pre className="min-h-0 flex-1 overflow-auto whitespace-pre-wrap break-all px-2 pb-2
                font-mono text-[11px] text-slate-700">
                {logs.lines.length === 0
                    ? 'No captured output. The hub keeps a bounded tail in memory; the file above has all of it.'
                    : logs.lines.join('\n')}
            </pre>
        </div>
    );
}

/** The worker's own persisted state. */
function SnapshotPane({ sessionId }: { sessionId: string }) {
    const client = useClient();
    const snapshot = usePanel((state) => state.snapshot);
    const [showReadable, setShowReadable] = useState(false);

    const mine = snapshot?.sessionId === sessionId ? snapshot : null;
    const view = mine?.view ?? null;

    return (
        <div className="flex h-full min-h-0 flex-col">
            <div className="flex shrink-0 items-center gap-2 px-2 py-1 text-[11px] text-slate-500">
                <Button
                    icon={<RefreshCw aria-hidden className="h-3 w-3" />}
                    disabled={mine?.loading === true}
                    onClick={() => { void client.loadSnapshot(sessionId); }}
                >
                    {mine?.loading ? 'loading…' : 'Load snapshot'}
                </Button>
                {view?.state_error && (
                    <span className="text-rose-700">state.json could not be parsed: {view.state_error}</span>
                )}
                <span className="flex-1" />
                {view?.readable && (
                    <Button onClick={() => setShowReadable((value) => !value)}>
                        {showReadable ? 'show state.json' : 'show readable.md'}
                    </Button>
                )}
            </div>

            <div className="min-h-0 flex-1 overflow-auto px-2 pb-2">
                {mine?.error && <p className="text-[11px] text-rose-700">{mine.error}</p>}
                {!mine && (
                    <p className="text-[11px] text-slate-500">
                        The worker's own persisted state, read from the hub's data directory and
                        never written by the panel.
                    </p>
                )}
                {mine && !view && !mine.error && !mine.loading && (
                    <p className="text-[11px] text-slate-500">Nothing loaded yet.</p>
                )}
                {view && showReadable && (
                    <pre className="whitespace-pre-wrap break-words font-mono text-[11px] text-slate-700">
                        {view.readable ?? '(no readable.md was written)'}
                    </pre>
                )}
                {view && !showReadable && (
                    <>
                        <p className="mb-1 text-[11px] text-slate-500">
                            {Object.keys(view.files).length === 0
                                ? 'No snapshot files exist for this session yet.'
                                : `files: ${Object.values(view.files).join(', ')}`}
                        </p>
                        <pre className="whitespace-pre-wrap break-all font-mono text-[11px] text-slate-700">
                            {view.state === null
                                ? '(no state.json)'
                                : prettyJson(view.state)}
                        </pre>
                    </>
                )}
            </div>
        </div>
    );
}

/** The drawer itself. */
export function Inspector() {
    const selected = usePanel((state) => state.selected);
    const session = useSession(selected);
    const open = usePanel((state) => state.inspectorOpen);
    const tab = usePanel((state) => state.inspectorTab);
    const setTab = usePanel((state) => state.setInspectorTab);
    const setOpen = usePanel((state) => state.setInspectorOpen);

    if (!open || !selected || !session) return null;

    return (
        <aside
            data-testid="inspector"
            aria-label="context drawer"
            className="flex w-96 shrink-0 flex-col border-l border-slate-200 bg-white"
        >
            <div className="flex items-center gap-1 px-2 pt-1">
                <span className="text-[11px] font-semibold uppercase tracking-wide text-slate-400">
                    context
                </span>
                <span className="flex-1" />
                <Button variant="ghost" onClick={() => setOpen(false)}>Close</Button>
            </div>
            <Tabs
                value={tab}
                onValueChange={(value) => setTab(value as InspectorTab)}
                className="flex min-h-0 flex-1 flex-col"
            >
                <TabsList label="context panes">
                    <TabsTrigger value="run">Run</TabsTrigger>
                    <TabsTrigger value="process">Process</TabsTrigger>
                    <TabsTrigger value="logs">Logs</TabsTrigger>
                    <TabsTrigger value="snapshot">Snapshot</TabsTrigger>
                </TabsList>

                {/* Only the selected panel is rendered, so a pane that was
                    visited cannot linger the way it did in the old inspector. */}
                <TabsPanel value="run"><RunPane sessionId={selected} /></TabsPanel>
                <TabsPanel value="process"><ProcessPane session={session} /></TabsPanel>
                <TabsPanel value="logs"><LogsPane sessionId={selected} /></TabsPanel>
                <TabsPanel value="snapshot"><SnapshotPane sessionId={selected} /></TabsPanel>
            </Tabs>
        </aside>
    );
}
