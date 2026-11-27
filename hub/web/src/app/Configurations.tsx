/** Manage saved library files separately from session configuration snapshots. */
import { useEffect, useRef, useState } from 'react';
import { useClient } from './ClientContext.tsx';
import { usePanel } from '../state/usePanel.ts';
import { Button } from '../ui/Button.tsx';
import { Dialog, DialogContent } from '../ui/overlays.tsx';

import type { ConfigKind as Kind, ConfigFile, ConfigList, ConfigPreviewResponse, ConfigTemplateResponse, RestOperations } from '../../../ui/index.ts';
export type { ConfigList } from '../../../ui/index.ts';
type FileSelection = Pick<ConfigFile, 'id' | 'revision'>;
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

export function Configurations({ open, onClose }: { open: boolean; onClose: () => void }) {
    const { rest } = useClient();
    const selected = usePanel(state => state.selected);
    const session = usePanel(state => selected ? state.sessions.get(selected) : undefined);
    const [list, setList] = useState<ConfigList>({ launch: [], worker: [] });
    const [templateSource, setTemplateSource] = useState('default');
    const [kind, setKind] = useState<Kind>('worker');
    const [file, setFile] = useState<FileSelection | null>(null);
    const [name, setName] = useState('');
    const [busy, setBusy] = useState(false);
    const [error, setError] = useState('');
    const [notice, setNotice] = useState('');
    const [launch, setLaunch] = useState('local');
    const [worker, setWorker] = useState('default');
    const [preview, setPreview] = useState<ConfigPreviewResponse['endpoints'] | null>(null);
    const stopped = session && !session.connected && !['starting', 'running', 'stopping'].includes(session.process?.state ?? '');

    useEffect(() => {
        if (!open) return;
        let disposed = false;
        rest.request<ConfigList>('GET', '/api/configurations').then(value => {
            if (!disposed) setList(value);
        }).catch(cause => { if (!disposed) setError(String(cause)); });
        return () => { disposed = true; };
    }, [open, rest]);

    async function action(work: () => Promise<void>) {
        setBusy(true); setError(''); setNotice('');
        try { await work(); }
        catch (cause) { setError(cause instanceof Error ? cause.message : String(cause)); }
        finally { setBusy(false); }
    }

    const refresh = async () => setList(await rest.request<ConfigList>('GET', '/api/configurations'));
    const path = (id: string) => `/api/configurations/${kind}/${encodeURIComponent(id)}`;
    const suffix = kind === 'launch' ? '.jsonc' : '.yaml';
    const read = (id: string) => rest.request<ConfigFile & { path: string }>('GET', path(id) + '/source');
    function select(value: ConfigFile) {
        setFile({ id: value.id, revision: value.revision });
        setName('');
    }
    async function save(id: string, text: string, revision: string | null) {
        const saved = await rest.request<ConfigFile>('PUT', path(id), { body: { text, revision } });
        select(saved);
        await refresh();
        setNotice(`Saved ${saved.id}${suffix} to the library.`);
    }
    async function upload(source: globalThis.File, replacement?: FileSelection) {
        if (source.size > 1024 * 1024) throw new Error('Configuration exceeds 1 MiB.');
        const id = replacement?.id ?? source.name.replace(/\.(yaml|yml|jsonc|json)$/i, '');
        if (!replacement && list[kind].includes(id)) {
            throw new Error(`Select ${id}${suffix} and choose Replace from file to update it.`);
        }
        if (replacement && !window.confirm(`Replace ${id}${suffix} with ${source.name}?`)) return;
        const text = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true }).decode(await source.arrayBuffer());
        await save(id, text, replacement?.revision ?? null);
    }
    async function download() {
        if (!file) return;
        const latest = await read(file.id);
        const url = URL.createObjectURL(new Blob([latest.text], { type: 'text/plain;charset=utf-8' }));
        const link = document.createElement('a');
        link.href = url;
        link.download = latest.id + suffix;
        link.click();
        window.setTimeout(() => URL.revokeObjectURL(url), 1000);
        select(latest);
    }
    async function showPreview() {
        const current = (await rest.request<ConfigFile>('GET', `/api/configurations/launch/${encodeURIComponent(launch)}/source`)).text;
        const result = await rest.request<ConfigPreviewResponse>('POST', '/api/configurations/preview', { body: { launch: current } satisfies RestOperations['POST /api/configurations/preview']['body'] });
        setPreview(result.endpoints);
    }

    return (
        <Dialog open={open} onOpenChange={value => { if (!value && !busy) onClose(); }}>
            <DialogContent title="Configurations" description="Manage reusable worker and launch configuration files." wide frosted>
                <div className="configuration-workspace space-y-4">
                    <div className="config-library flex flex-wrap items-center gap-2">
                        <select aria-label="Configuration kind" className={control} value={kind} disabled={busy}
                            onChange={e => { setKind(e.target.value as Kind); setTemplateSource('default'); setFile(null); setName(''); setPreview(null); setError(''); setNotice(''); }}>
                            <option value="worker">Worker · YAML</option><option value="launch">Launch · JSONC</option>
                        </select>
                        <select aria-label="Saved configuration" className={control} value={file?.id ?? ''} disabled={busy}
                            onChange={e => { const id = e.target.value; setFile(null); setName(''); if (id) void action(async () => select(await read(id))); }}>
                            <option value="">Select a saved file</option>
                            {list[kind].map(id => <option key={id} value={id}>{id}{suffix}</option>)}
                        </select>
                        <Button disabled={busy} onClick={() => void action(async () => {
                            await refresh();
                            if (file) select(await read(file.id));
                        })}>Refresh</Button>
                    </div>
                    {file && <div className="flex flex-wrap items-center gap-2">
                        {list.editorAvailable && <Button variant="primary" disabled={busy} onClick={() => void action(async () => {
                            await rest.request('POST', path(file.id) + '/open', { body: {} });
                            setNotice('Requested editor on the Hub host.');
                        })}>Open in editor</Button>}
                        <Button disabled={busy} onClick={() => void action(download)}>Download</Button>
                        <Button disabled={busy} onClick={() => void action(async () => {
                            const { path } = await read(file.id);
                            setNotice(path);
                            await navigator.clipboard?.writeText(path).catch(() => { /* Path stays available to copy manually. */ });
                        })}>Copy Hub path</Button>
                        <UploadButton label="Replace from file" disabled={busy} accept={kind === 'worker' ? '.yaml,.yml' : '.jsonc,.json'}
                            onFile={source => void action(() => upload(source, file))} />
                        <Button disabled={busy} onClick={() => void action(async () => {
                            const latest = await read(file.id);
                            await rest.request('POST', `/api/configurations/${kind}/validate`, { body: { text: latest.text } });
                            setNotice('Valid configuration. The worker checks plugins and local resources at startup.');
                        })}>Validate</Button>
                        <Button variant="ghost" className="text-danger" disabled={busy} onClick={() => {
                            if (window.confirm(`Delete ${file.id}${suffix} from the library?`)) void action(async () => {
                                await rest.request('DELETE', path(file.id), { body: { revision: file.revision } });
                                setFile(null); await refresh();
                            });
                        }}>Delete</Button>
                    </div>}
                    <div className="flex flex-wrap items-center gap-2 border-t border-line pt-3">
                        <input aria-label="Configuration name" className={control} value={name} placeholder="New file name"
                            disabled={busy} onChange={e => setName(e.target.value)} maxLength={128} />
                        {kind === 'launch' && <select aria-label="Template source" className={control} value={templateSource} disabled={busy} onChange={e => setTemplateSource(e.target.value)}>
                            <option value="default">Default template</option>
                            <option value="docker">Docker</option><option value="managed">Managed worker</option>
                        </select>}
                        <Button disabled={busy || !name} onClick={() => void action(async () => {
                            const template = await rest.request<ConfigTemplateResponse>('GET', `/api/configurations/${kind}/template?source=${templateSource}`);
                            await save(name, template.text, null);
                        })}>New from template</Button>
                        {file && <>
                            <Button disabled={busy || !name || name === file.id} onClick={() => void action(async () => {
                                await save(name, (await read(file.id)).text, null);
                            })}>Save copy</Button>
                            <Button disabled={busy || !name || name === file.id} onClick={() => void action(async () => {
                                select(await rest.request<ConfigFile>('POST', path(file.id) + '/rename', { body: { id: name, revision: file.revision } }));
                                await refresh();
                            })}>Rename</Button>
                        </>}
                        <UploadButton label="Upload new file" disabled={busy} accept={kind === 'worker' ? '.yaml,.yml' : '.jsonc,.json'}
                            onFile={source => void action(() => upload(source))} />
                    </div>
                    {error && <p role="alert" className="break-words text-sm text-danger">{error}</p>}
                    {notice && <p role="status" className="text-xs text-ok">{notice}</p>}
                    <section className="space-y-3 border-t border-line pt-3" aria-label="Session configuration">
                    <div className="flex flex-wrap items-center justify-between gap-2">
                        <h3 className="text-xs font-semibold text-ink">Session configuration</h3>
                        <span className="text-xs text-ink-muted">{selected ? `Apply saved files to ${selected}` : 'Select a session to apply saved files'}</span>
                    </div>
                    <ConfigurationChoices list={list} launch={launch} worker={worker} onLaunch={setLaunch} onWorker={setWorker} disabled={busy} />
                    <div className="flex flex-wrap gap-2">
                        <Button disabled={busy || !launch} onClick={() => void action(showPreview)}>Preview Hub endpoints</Button>
                        <Button disabled={busy || !stopped || !launch || !worker} onClick={() => {
                            if (!selected || !window.confirm(`Replace saved configuration for ${selected}? Its conversation state is retained.`)) return;
                            void action(async () => {
                                await rest.request('POST', `/api/sessions/${encodeURIComponent(selected)}/configurations`, { body: { launchConfig: launch, workerConfig: worker } satisfies RestOperations['POST /api/sessions/:id/configurations']['body'] });
                                setNotice(`Configuration applied to ${selected}. Start its worker to load it.`);
                            });
                        }}>Apply to {selected ?? 'selected session'}</Button>
                        <Button className="ml-auto" disabled={busy} onClick={onClose}>Close</Button>
                    </div>
                    <p className="text-xs text-ink-muted">Apply requires a stopped, disconnected worker and uses saved files. A bound managed session keeps its captured configuration.</p>
                    </section>
                    {preview && <div className="overflow-auto rounded border border-line bg-surface p-2 text-xs">
                        <p className="mb-1 text-ink-muted">Session identity is filled at launch. For containers or proxies, edit the launch config address settings.</p>
                        {Object.entries(preview).map(([key, value]) => <p key={key} className="break-all font-mono">{key}: {value}</p>)}
                    </div>}
                </div>
            </DialogContent>
        </Dialog>
    );
}

/** Select a local text file. Reset the picker so a corrected file can be retried. */
function UploadButton({ label, disabled, accept, onFile }: {
    label: string; disabled: boolean; accept: string; onFile: (file: globalThis.File) => void;
}) {
    const input = useRef<HTMLInputElement>(null);
    return <>
        <input hidden ref={input} aria-label={label} type="file" accept={accept} disabled={disabled}
            onChange={event => {
                const chosen = event.currentTarget.files?.[0];
                event.currentTarget.value = '';
                if (chosen) onFile(chosen);
            }} />
        <Button disabled={disabled} onClick={() => input.current?.click()}>{label}</Button>
    </>;
}
