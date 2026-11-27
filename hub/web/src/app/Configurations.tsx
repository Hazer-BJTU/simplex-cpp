/** Manage saved library files separately from session configuration snapshots. */
import { useEffect, useRef, useState } from 'react';
import { useClient } from './ClientContext.tsx';
import { usePanel } from '../state/usePanel.ts';
import { Button, IconButton } from '../ui/Button.tsx';
import { ConfigurationFileGroup, libraryFilename, entryId, type FileAction, type LibraryFile, type LibraryTask } from './ConfigurationFileGroup.tsx';
import { Glyph } from '../ui/icons.tsx';
import { Dialog, DialogContent } from '../ui/overlays.tsx';
import { ApiError } from '../lib/rest.ts';

import type { ConfigFile, ConfigList, ConfigPreviewResponse, ConfigTemplateResponse, SessionDescription } from '../../../ui/index.ts';
export type { ConfigList } from '../../../ui/index.ts';
const control = 'config-control h-9 min-w-0 rounded-md border border-line bg-surface px-3 text-sm text-ink transition-colors hover:border-line-strong';

/** Shared picker for session creation and explicit snapshot replacement. */
export function ConfigurationChoices({ list, launch, worker, onLaunch, onWorker, disabled = false }: {
    list: ConfigList;
    launch: string;
    worker: string;
    onLaunch: (value: string) => void;
    onWorker: (value: string) => void;
    disabled?: boolean;
}) {
    return (
        <div className="grid gap-3 sm:grid-cols-2">
            <label className="flex min-w-0 flex-col gap-1 text-xs text-ink-muted">Launch configuration
                <select aria-label="Launch configuration" className={control} value={launch} disabled={disabled} onChange={e => onLaunch(e.target.value)}>
                    <option value="">Select configuration</option>
                    {list.launch.map(id => <option key={id}>{id}</option>)}
                </select>
            </label>
            <label className="flex min-w-0 flex-col gap-1 text-xs text-ink-muted">Worker configuration
                <select aria-label="Worker configuration" className={control} value={worker} disabled={disabled} onChange={e => onWorker(e.target.value)}>
                    <option value="">Select configuration</option>
                    {list.worker.map(id => <option key={id}>{id}</option>)}
                </select>
            </label>
        </div>
    );
}

const emptyList: ConfigList = { worker: [], launch: [] };
const filePath = (file: LibraryFile) => `/api/configurations/${file.kind}/${encodeURIComponent(file.id)}`;

async function uploadedText(source: File): Promise<string> {
    if (source.size > 1024 * 1024) throw new Error('Configuration exceeds 1 MiB.');
    return new TextDecoder('utf-8', { fatal: true, ignoreBOM: true }).decode(await source.arrayBuffer());
}

type RunAction = (work: () => Promise<string | void>) => Promise<void>;

/** Remember observed revisions across dialog closes while discarding transient forms. */
export function Configurations({ open, onClose }: { open: boolean; onClose: () => void }) {
    const revisions = useRef(new Map<string, string>());
    return open ? <ConfigurationManager revisions={revisions.current} onClose={onClose} /> : null;
}

function ConfigurationManager({ revisions, onClose }: { revisions: Map<string, string>; onClose: () => void }) {
    const { rest } = useClient();
    const selected = usePanel(state => state.selected);
    const session = usePanel(state => selected ? state.sessions.get(selected) : undefined);
    const [list, setList] = useState<ConfigList | null>(null);
    const [task, setTask] = useState<LibraryTask | null>(null);
    const [loading, setLoading] = useState(true);
    const [busy, setBusy] = useState(false);
    const [feedback, setFeedback] = useState<{ text: string; error?: boolean } | null>(null);

    useEffect(() => {
        const controller = new AbortController();
        rest.request<ConfigList>('GET', '/api/configurations', { signal: controller.signal })
            .then(value => { if (!controller.signal.aborted) setList(value); })
            .catch(error => { if (!controller.signal.aborted) setFeedback({ text: error.message, error: true }); })
            .finally(() => { if (!controller.signal.aborted) setLoading(false); });
        return () => controller.abort();
    }, [rest]);

    const run: RunAction = async work => {
        if (busy) return;
        setBusy(true);
        setFeedback(null);
        try {
            const text = await work();
            if (text) setFeedback({ text });
        } catch (error) {
            setFeedback({ text: error instanceof Error ? error.message : String(error), error: true });
        } finally {
            setBusy(false);
        }
    };
    const refresh = async () => setList(await rest.request<ConfigList>('GET', '/api/configurations'));
    const read = (file: LibraryFile) => rest.request<ConfigFile & { path: string }>('GET', filePath(file) + '/source');
    // Downloading pins the content the operator took away. Listing files or acting on
    // another row must never silently advance the revision used to replace that copy.
    const replacementRevision = async (file: LibraryFile) => revisions.get(filePath(file)) ?? (await read(file)).revision;

    function finishTask(file: { kind: LibraryFile['kind']; id?: string }) {
        setTask(null);
        setFeedback(null);
        requestAnimationFrame(() => {
            const row = document.getElementById(entryId(file.kind, file.id)) ?? document.getElementById(entryId(file.kind));
            row?.querySelector<HTMLButtonElement>('button[aria-haspopup="menu"]')?.focus();
        });
    }

    /** Publish the acknowledged change to its own group; refresh picks up external edits. */
    function updateListing(kind: LibraryFile['kind'], add: string | null, remove: string | null) {
        setList(current => {
            if (!current) return current;
            const names = new Set(current[kind]);
            if (remove !== null) names.delete(remove);
            if (add !== null) names.add(add);
            return { ...current, [kind]: [...names].sort() };
        });
    }

    function saved(file: ConfigFile, oldName: string | null = null) {
        revisions.set(filePath(file), file.revision);
        updateListing(file.kind, file.id, oldName);
        finishTask(file);
        return `Saved ${libraryFilename(file)}.`;
    }

    async function save(file: LibraryFile, text: string, previous: string | null) {
        return saved(await rest.request<ConfigFile>('PUT', filePath(file), { body: { text, revision: previous } }));
    }

    async function fileAction(file: LibraryFile, action: FileAction) {
        setTask(null);
        const path = filePath(file);
        switch (action) {
            case 'open':
                await rest.request('POST', path + '/open', { body: {} });
                return `Requested editor for ${libraryFilename(file)} on the Hub host.`;
            case 'download': {
                const latest = await read(file);
                const url = URL.createObjectURL(new Blob([latest.text], { type: 'text/plain;charset=utf-8' }));
                const link = document.createElement('a');
                link.href = url;
                link.download = libraryFilename(file);
                link.click();
                window.setTimeout(() => URL.revokeObjectURL(url), 1000);
                revisions.set(path, latest.revision);
                return;
            }
            case 'rename':
                setTask({ ...file, action, revision: (await read(file)).revision });
                return;
            case 'replace':
                setTask({ ...file, action, revision: await replacementRevision(file) });
                return;
            case 'copy':
                setTask({ ...file, action });
                return;
            case 'path': {
                const source = await read(file);
                await navigator.clipboard?.writeText(source.path).catch(() => { /* Display for manual copying. */ });
                return source.path;
            }
            case 'validate':
                await rest.request('POST', `/api/configurations/${file.kind}/validate`, { body: { text: (await read(file)).text } });
                return `${libraryFilename(file)} is valid. Worker startup checks plugins and local resources.`;
            case 'preview': {
                const result = await rest.request<ConfigPreviewResponse>('POST', '/api/configurations/preview', {
                    body: { launch: (await read(file)).text },
                });
                setTask({ ...file, action, endpoints: result.endpoints });
                return;
            }
            case 'delete': {
                const previous = (await read(file)).revision;
                if (!window.confirm(`Delete ${libraryFilename(file)} from the library?`)) return;
                await rest.request('DELETE', path, { body: { revision: previous } });
                revisions.delete(path);
                updateListing(file.kind, null, file.id);
                finishTask({ kind: file.kind });
                return `Deleted ${libraryFilename(file)}.`;
            }
        }
    }

    async function submitTask(values: FormData) {
        if (!task || task.action === 'preview') return;
        if (task.action === 'replace') {
            const file = values.get('upload');
            if (!(file instanceof File)) throw new Error('Choose a replacement file.');
            try {
                return await save(task, await uploadedText(file), task.revision);
            } catch (error) {
                if (error instanceof ApiError && error.status === 409) {
                    throw new Error(`${libraryFilename(task)} changed. Download the current file and reconcile your changes before replacing it.`);
                }
                throw error;
            }
        }
        const id = String(values.get('name'));
        const target = { kind: task.kind, id };
        switch (task.action) {
            case 'create': {
                const template = await rest.request<ConfigTemplateResponse>('GET', `/api/configurations/${task.kind}/template`,
                    { query: { source: String(values.get('template') ?? 'default') } });
                return save(target, template.text, null);
            }
            case 'copy':
                return save(target, (await read(task)).text, null);
            case 'rename': {
                if (task.id === id) { finishTask(task); return; }
                const renamed = await rest.request<ConfigFile>('POST', filePath(task) + '/rename', {
                    body: { id, revision: task.revision },
                });
                revisions.delete(filePath(task));
                return saved(renamed, task.id);
            }
        }
    }

    const close = () => { if (!busy) onClose(); };
    return (
        <Dialog open onOpenChange={value => {
            if (value || busy) return;
            if (task) finishTask(task);
            else onClose();
        }}>
            <DialogContent title="Configurations" wide frosted focus="self"
                headerActions={<div className="flex gap-1">
                    <IconButton label="Refresh configurations" disabled={busy || loading} onClick={() => void run(async () => {
                        await refresh();
                        setTask(null);
                    })}><Glyph name={busy || loading ? 'spinner' : 'refresh'} /></IconButton>
                    <IconButton label="Close" disabled={busy} onClick={close}><Glyph name="close" /></IconButton>
                </div>}
                footer={feedback && <p role={feedback.error ? 'alert' : 'status'}
                    className={`max-h-24 overflow-y-auto break-words text-xs ${feedback.error ? 'text-danger' : 'text-ink-muted'}`}>{feedback.text}</p>}>
                <div className="space-y-4">
                    {list ? (['worker', 'launch'] as const).map(kind => (
                        <ConfigurationFileGroup key={kind} kind={kind} ids={list[kind]} editorAvailable={!!list.editorAvailable}
                            busy={busy} task={task?.kind === kind ? task : null}
                            onNew={() => { setTask({ action: 'create', kind }); setFeedback(null); }}
                            onCancel={() => { if (task) finishTask(task); }}
                            onSubmit={values => void run(() => submitTask(values))}
                            onAction={(file, action) => void run(() => fileAction(file, action))}
                            onUpload={source => void run(async () => {
                                const id = source.name.replace(/\.(yaml|yml|jsonc|json)$/i, '');
                                if (list[kind].includes(id)) throw new Error(`Use Replace from file on ${libraryFilename({ kind, id })} to update it.`);
                                return save({ kind, id }, await uploadedText(source), null);
                            })} />
                    )) : (busy || loading) && <p className="text-xs text-ink-muted">Loading configurations…</p>}
                    {session && session.kind !== 'headless' && <SessionConfiguration key={session.session_id}
                        session={session} list={list ?? emptyList} busy={busy} run={run} />}
                </div>
            </DialogContent>
        </Dialog>
    );
}

/** A library operation never changes these selectors or applies files to a session. */
function SessionConfiguration({ session, list, busy, run }: {
    session: SessionDescription; list: ConfigList; busy: boolean; run: RunAction;
}) {
    const { rest } = useClient();
    const [launch, setLaunch] = useState(session.spec.launchConfig ?? 'local');
    const [worker, setWorker] = useState(session.spec.workerConfig ?? 'default');
    const stopped = !session.connected && !['starting', 'running', 'stopping'].includes(session.process?.state ?? '');
    const id = session.session_id;
    return (
        <details aria-label="Session configuration" className="group border-t border-line pt-2">
            <summary className="flex min-h-9 cursor-pointer list-none items-center gap-2 rounded text-sm focus-visible:outline-interactive">
                <Glyph name="chevron-right" className="group-open:rotate-90" />
                <span>Session configuration</span><span className="truncate font-mono text-xs text-ink-muted">{id}</span>
            </summary>
            <div className="space-y-3 py-2">
                <ConfigurationChoices list={list} launch={launch} worker={worker} onLaunch={setLaunch} onWorker={setWorker} disabled={busy} />
                <p className="text-xs text-ink-muted">Apply to a stopped, disconnected session. A bound managed session keeps its captured configuration.</p>
                <Button disabled={busy || !stopped || !list.launch.includes(launch) || !list.worker.includes(worker)} onClick={() => {
                    if (!window.confirm(`Apply ${launch}.jsonc and ${worker}.yaml to ${id}? Its conversation state is retained.`)) return;
                    void run(async () => {
                        await rest.request('POST', `/api/sessions/${encodeURIComponent(id)}/configurations`, { body: { launchConfig: launch, workerConfig: worker } });
                        return `Configuration applied to ${id}. Start its worker to load it.`;
                    });
                }}>Apply to {id}</Button>
            </div>
        </details>
    );
}
