/** Configuration library UI. Saving a shared file does not mutate a session;
 * applying to a stopped session is an explicit separate action. */
import { useEffect, useState } from 'react';
import { useClient } from './ClientContext.tsx';
import { usePanel } from '../state/usePanel.ts';
import { Button } from '../ui/Button.tsx';
import { Dialog, DialogContent } from '../ui/overlays.tsx';
import { ConfigEditor } from './ConfigEditor.tsx';

import type { ConfigKind as Kind, ConfigFile as File, ConfigList } from '../../../shared/configurations.ts';
export type { ConfigList } from '../../../shared/configurations.ts';
const control = 'rounded border border-line-strong bg-surface px-2 py-1.5 text-sm text-ink';

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
    const [file, setFile] = useState<File | null>(null);
    const [text, setText] = useState('');
    const [name, setName] = useState('');
    const [busy, setBusy] = useState(false);
    const [error, setError] = useState('');
    const [notice, setNotice] = useState('');
    const [launch, setLaunch] = useState('local');
    const [worker, setWorker] = useState('default');
    const [preview, setPreview] = useState<Record<string, string> | null>(null);
    const dirty = text !== (file?.text ?? '');
    const stopped = session && !session.connected && !['starting', 'running', 'stopping'].includes(session.process?.state ?? '');

    useEffect(() => {
        if (!open) return;
        let disposed = false;
        rest.request<ConfigList>('GET', '/api/configurations').then(value => {
            if (!disposed) setList(value);
        }).catch(cause => { if (!disposed) setError(String(cause)); });
        return () => { disposed = true; };
    }, [open, rest]);

    useEffect(() => {
        if (!dirty) return;
        const warn = (event: BeforeUnloadEvent) => { event.preventDefault(); };
        window.addEventListener('beforeunload', warn);
        return () => window.removeEventListener('beforeunload', warn);
    }, [dirty]);

    async function action(work: () => Promise<void>) {
        setBusy(true); setError(''); setNotice('');
        try { await work(); }
        catch (cause) { setError(cause instanceof Error ? cause.message : String(cause)); }
        finally { setBusy(false); }
    }

    const discard = () => !dirty || window.confirm('Discard unsaved configuration changes?');
    const refresh = async () => setList(await rest.request<ConfigList>('GET', '/api/configurations'));
    function loaded(value: File) { setFile(value); setText(value.text); setName(value.id); setKind(value.kind); }
    const path = (id: string) => `/api/configurations/${kind}/${encodeURIComponent(id)}`;

    async function showPreview() {
        const current = kind === 'launch' && text ? text :
            (await rest.request<File>('GET', `/api/configurations/launch/${encodeURIComponent(launch)}`)).text;
        const result = await rest.request<{ endpoints: Record<string, string> }>('POST', '/api/configurations/preview', { body: { launch: current } });
        setPreview(result.endpoints);
    }

    return (
        <Dialog open={open} onOpenChange={value => { if (!value && !busy && discard()) onClose(); }}>
            <DialogContent title="Configurations" description="Edit reusable launch and worker files. Session snapshots change only when explicitly applied." wide>
                <div className="space-y-3">
                    <div className="flex flex-wrap items-center gap-2">
                        <select aria-label="Configuration kind" className={control} value={kind} disabled={busy}
                            onChange={e => { if (discard()) { setKind(e.target.value as Kind); setFile(null); setText(''); setName(''); setPreview(null); } }}>
                            <option value="launch">Launch configs · JSONC</option><option value="worker">Worker configs · YAML</option>
                        </select>
                        <select aria-label="Saved configuration" className={control} value={file?.id ?? ''} disabled={busy}
                            onChange={e => { const id = e.target.value; if (id && discard()) void action(async () => loaded(await rest.request<File>('GET', path(id)))); }}>
                            <option value="">Select a saved file</option>
                            {list[kind].map(id => <option key={id}>{id}</option>)}
                        </select>
                        <select aria-label="Template source" className={control} value={templateSource} disabled={busy} onChange={e => setTemplateSource(e.target.value)}>
                            <option value="default">Default template</option>
                            <option value="deployment">Current Hub deployment</option>
                        </select>
                        <Button disabled={busy} onClick={() => {
                            if (!discard()) return;
                            void action(async () => {
                                const result = await rest.request<{ text: string }>('GET', `/api/configurations/${kind}/template?source=${templateSource}`);
                                setFile(null); setText(result.text); setName(''); setPreview(null);
                            });
                        }}>New from template</Button>
                    </div>
                    <div className="flex flex-wrap items-center gap-2">
                        <input aria-label="Configuration name" className={control} value={name} placeholder="Configuration name"
                            disabled={busy} onChange={e => setName(e.target.value)} maxLength={128} />
                        <span className="text-xs text-ink-muted">{kind === 'launch' ? '.jsonc' : '.yaml'}{dirty ? ' · Unsaved changes' : ''}</span>
                        <Button variant="primary" disabled={busy || !name || !text} onClick={() => void action(async () => {
                            const value = await rest.request<File>('PUT', path(name), { body: { text, revision: file?.id === name ? file.revision : null } });
                            loaded(value); await refresh(); setNotice('Saved. Existing session snapshots are unchanged.');
                        })}>{file && file.id !== name ? 'Save copy' : 'Save'}</Button>
                        <Button disabled={busy || !text} onClick={() => void action(async () => {
                            await rest.request('POST', `/api/configurations/${kind}/validate`, { body: { text } });
                            setNotice('Configuration syntax and required fields are valid. Worker startup checks plugins and local resources.');
                        })}>Validate</Button>
                        <Button disabled={busy || !file} onClick={() => { if (file && discard()) void action(async () => loaded(await rest.request<File>('GET', path(file.id)))); }}>Reload</Button>
                        <Button disabled={busy || !file || file.id === name || !name || dirty} onClick={() => void action(async () => {
                            if (!file) return;
                            loaded(await rest.request<File>('POST', `${path(file.id)}/rename`, { body: { id: name, revision: file.revision } }));
                            await refresh();
                        })}>Rename</Button>
                        <Button variant="danger" disabled={busy || !file} onClick={() => {
                            if (!file || !window.confirm(`Delete ${file.id}? Existing sessions keep their snapshots.`)) return;
                            void action(async () => {
                                await rest.request('DELETE', path(file.id), { body: { revision: file.revision } });
                                setFile(null); setText(''); setName(''); await refresh();
                            });
                        }}>Delete</Button>
                    </div>
                    <ConfigEditor text={text} language={kind === 'worker' ? 'yaml' : 'json'} onChange={setText} disabled={busy} />
                    {error && <p role="alert" className="break-words text-sm text-danger">{error}</p>}
                    {notice && <p role="status" className="text-xs text-ok">{notice}</p>}
                    <ConfigurationChoices list={list} launch={launch} worker={worker} onLaunch={setLaunch} onWorker={setWorker} disabled={busy} />
                    <div className="flex flex-wrap gap-2">
                        <Button disabled={busy || !launch} onClick={() => void action(showPreview)}>Preview Hub endpoints</Button>
                        <Button disabled={busy || !stopped || !launch || !worker} onClick={() => {
                            if (!selected || !window.confirm(`Replace saved configuration for ${selected}? Its conversation state is retained.`)) return;
                            void action(async () => {
                                await rest.request('POST', `/api/sessions/${encodeURIComponent(selected)}/configurations`, { body: { launchConfig: launch, workerConfig: worker } });
                                setNotice(`Configuration applied to ${selected}. Start its worker to load it.`);
                            });
                        }}>Apply to {selected ?? 'selected session'}</Button>
                        <Button className="ml-auto" disabled={busy} onClick={() => { if (discard()) onClose(); }}>Close</Button>
                    </div>
                    <p className="text-xs text-ink-muted">Apply requires a stopped, disconnected worker and uses saved files. To copy a file, change its name and save.</p>
                    {preview && <div className="overflow-auto rounded border border-line bg-surface p-2 text-xs">
                        <p className="mb-1 text-ink-muted">Session identity is filled at launch. For containers or proxies, edit the launch config address settings.</p>
                        {Object.entries(preview).map(([key, value]) => <p key={key} className="break-all font-mono">{key}: {value}</p>)}
                    </div>}
                </div>
            </DialogContent>
        </Dialog>
    );
}
