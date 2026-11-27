import { useLayoutEffect, useRef } from 'react';
import type { ConfigFile, ConfigKind, ConfigPreviewResponse } from '../../../ui/index.ts';
import { Button, IconButton } from '../ui/Button.tsx';
import { Glyph } from '../ui/icons.tsx';
import { Menu, MenuContent, MenuItem, MenuSeparator, MenuTrigger } from '../ui/overlays.tsx';

export type LibraryFile = Pick<ConfigFile, 'kind' | 'id'>;
export type FileAction = 'open' | 'download' | 'replace' | 'validate' | 'path' | 'copy' | 'rename' | 'preview' | 'delete';

/** Only the active action owns form controls. Mutations hold the revision observed on entry. */
export type LibraryTask =
    | { action: 'create'; kind: ConfigKind; id?: never }
    | (LibraryFile & { action: 'copy' })
    | (LibraryFile & { action: 'rename' | 'replace'; revision: string })
    | (LibraryFile & { action: 'preview'; endpoints: ConfigPreviewResponse['endpoints'] });

export function libraryFilename(file: LibraryFile): string {
    return file.id + (file.kind === 'worker' ? '.yaml' : '.jsonc');
}

export function entryId(kind: ConfigKind, id?: string): string {
    return id === undefined ? `config-add-${kind}` : `config-file-${kind}-${id}`;
}

type InlineProps = {
    busy: boolean;
    onCancel: () => void;
    onSubmit: (values: FormData) => void;
};

function InlineAction({ task, busy, onCancel, onSubmit }: InlineProps & { task: LibraryTask }) {
    const fieldId = entryId(task.kind, task.id) + ':field';
    if (task.action === 'preview') {
        return <div className="space-y-2 px-3 pb-3 text-xs">
            <p className="text-ink-muted">Session identity is filled at launch.</p>
            <dl className="grid grid-cols-[auto_minmax(0,1fr)] gap-x-3 gap-y-1">
                {Object.entries(task.endpoints).map(([channel, url]) => (
                    <div key={channel} className="contents">
                        <dt className="text-ink-muted">{channel}</dt><dd className="break-all font-mono">{url}</dd>
                    </div>
                ))}
            </dl>
            <Button autoFocus id={fieldId} variant="ghost" onClick={onCancel}>Close preview</Button>
        </div>;
    }

    const isNew = task.action === 'create';
    // Leave room for "-copy" inside the library's 128-character name limit.
    const initialName = task.action === 'copy' ? task.id.slice(0, 123) + '-copy' : task.id ?? '';
    const label = isNew ? `New ${task.kind} configuration` : {
        copy: 'Create a copy of', rename: 'Rename', replace: 'Replace',
    }[task.action] + ' ' + libraryFilename(task);
    return (
        <form aria-label={label} className="space-y-2 px-3 py-3"
            onSubmit={event => {
                event.preventDefault();
                onSubmit(new FormData(event.currentTarget));
            }}>
            <label htmlFor={fieldId} className="block break-all text-xs font-medium text-ink">{label}</label>
            <div className="flex flex-wrap items-center gap-2">
                {task.action === 'replace' ? (
                    <input autoFocus id={fieldId} name="upload" type="file" required disabled={busy}
                        accept={task.kind === 'worker' ? '.yaml,.yml' : '.jsonc,.json'}
                        className="min-w-0 flex-auto text-xs file:mr-2 file:rounded file:bg-surface file:px-2 file:py-2" />
                ) : (
                    <input autoFocus id={fieldId} name="name" required maxLength={128} disabled={busy}
                        pattern={String.raw`[A-Za-z0-9_\-]{1,128}`}
                        title="Use letters, digits, underscores or hyphens."
                        placeholder="File name" onFocus={e => e.target.select()}
                        defaultValue={initialName}
                        className="config-control h-9 min-w-32 flex-1 rounded border border-line bg-surface px-2 text-sm" />
                )}
                {isNew && task.kind === 'launch' && (
                    <select aria-label="Launch template" name="template" disabled={busy}
                        className="config-control h-9 rounded border border-line bg-surface px-2 text-xs">
                        <option value="default">Local</option>
                        <option value="docker">Docker</option>
                        <option value="managed">Managed</option>
                    </select>
                )}
                <Button type="submit" variant="primary" disabled={busy}>
                    {{ create: 'Create', copy: 'Create copy', rename: 'Rename', replace: 'Replace' }[task.action]}
                </Button>
                <Button variant="ghost" disabled={busy} onClick={onCancel}>Cancel</Button>
            </div>
        </form>
    );
}

/** A group manages one file kind. Its picker stays mounted when the Add menu closes. */
export function ConfigurationFileGroup({ kind, ids, editorAvailable, busy, task, onNew, onUpload,
    onAction, onCancel, onSubmit }: InlineProps & {
    kind: ConfigKind;
    ids: readonly string[];
    editorAvailable: boolean;
    task: LibraryTask | null;
    onNew: () => void;
    onUpload: (file: File) => void;
    onAction: (file: LibraryFile, action: FileAction) => void;
}) {
    const picker = useRef<HTMLInputElement>(null);
    const deferredFocus = useRef<HTMLButtonElement | null>(null);
    useLayoutEffect(() => {
        if (busy) return;
        const origin = deferredFocus.current;
        deferredFocus.current = null;
        if (!origin?.isConnected) return;
        const active = document.activeElement;
        if (active && active !== document.body && !active.matches('[role="dialog"]')) return;
        const field = task && document.getElementById(entryId(task.kind, task.id) + ':field');
        (field ?? origin).focus();
    }, [busy, task]);

    function onMenuClose(event: Event, entry: string) {
        const trigger = document.getElementById(entry)?.querySelector<HTMLButtonElement>('button[aria-haspopup="menu"]');
        if (trigger?.disabled) {
            // Radix cannot return focus until the pending file operation re-enables its trigger.
            event.preventDefault();
            deferredFocus.current = trigger;
            return;
        }
        const field = document.getElementById(entry + ':field');
        if (field) {
            event.preventDefault();
            field.focus();
        }
    }

    const title = kind === 'worker' ? 'Worker configurations' : 'Launch configurations';
    const addId = entryId(kind);
    const form = task && <InlineAction key={task.action + ':' + (task.id ?? '')} task={task} busy={busy}
        onCancel={onCancel} onSubmit={onSubmit} />;
    return (
        <section aria-label={title} className="min-w-0">
            <div id={addId} className="flex items-center justify-between border-b border-line pb-1">
                <h3 className="text-sm font-semibold text-ink">{title}</h3>
                <Menu>
                    <MenuTrigger asChild disabled={busy}>
                        <Button variant="ghost" aria-label={`Add ${kind} configuration`}>+ Add</Button>
                    </MenuTrigger>
                    <MenuContent onCloseAutoFocus={event => onMenuClose(event, addId)}>
                        <MenuItem onSelect={onNew}>New from template…</MenuItem>
                        <MenuItem onSelect={() => picker.current?.click()}>Upload file…</MenuItem>
                    </MenuContent>
                </Menu>
                <input hidden type="file" ref={picker} accept={kind === 'worker' ? '.yaml,.yml' : '.jsonc,.json'}
                    aria-label={`Upload new ${kind} file`} onChange={event => {
                        const chosen = event.currentTarget.files?.[0];
                        event.currentTarget.value = '';
                        if (chosen) onUpload(chosen);
                    }} />
            </div>
            {task?.action === 'create' && form}
            {ids.length === 0 && <p className="px-2 py-3 text-xs text-ink-muted">No {kind} configurations.</p>}
            <ul className="divide-y divide-line">
                {ids.map(id => {
                    const file = { kind, id };
                    const filename = libraryFilename(file);
                    const trigger = entryId(kind, id);
                    return (
                        <li key={id} id={trigger} aria-label={filename}>
                            <div className="flex min-h-11 items-center gap-1 rounded px-1 hover:bg-subtle focus-within:bg-subtle">
                                <button type="button" disabled={busy} title={filename}
                                    aria-label={editorAvailable ? `Open ${filename} in editor` : `Download ${filename}`}
                                    onClick={event => {
                                        deferredFocus.current = event.currentTarget;
                                        onAction(file, editorAvailable ? 'open' : 'download');
                                    }}
                                    className="flex min-h-11 min-w-0 flex-1 items-center gap-3 rounded px-2 text-left focus-visible:outline-2 focus-visible:outline-interactive">
                                    <Glyph name="output" className="text-ink-muted" />
                                    <span className="min-w-0 flex-1 truncate font-mono text-sm">{filename}</span>
                                    <span className="text-xs text-ink-muted">{editorAvailable ? 'Open' : 'Download'}</span>
                                </button>
                                <Menu>
                                    <MenuTrigger asChild disabled={busy}>
                                        <IconButton label={`Actions for ${filename}`} size="md">
                                            <Glyph name="more" />
                                        </IconButton>
                                    </MenuTrigger>
                                    <MenuContent onCloseAutoFocus={event => onMenuClose(event, trigger)}>
                                        {editorAvailable && <MenuItem onSelect={() => onAction(file, 'download')}>Download</MenuItem>}
                                        <MenuItem onSelect={() => onAction(file, 'replace')}>Replace from file…</MenuItem>
                                        <MenuItem onSelect={() => onAction(file, 'validate')}>Validate</MenuItem>
                                        <MenuItem onSelect={() => onAction(file, 'path')}>Copy Hub path</MenuItem>
                                        {kind === 'launch' && <MenuItem onSelect={() => onAction(file, 'preview')}>Preview Hub endpoints</MenuItem>}
                                        <MenuSeparator />
                                        <MenuItem onSelect={() => onAction(file, 'copy')}>Create copy…</MenuItem>
                                        <MenuItem onSelect={() => onAction(file, 'rename')}>Rename…</MenuItem>
                                        <MenuSeparator />
                                        <MenuItem danger onSelect={() => onAction(file, 'delete')}>Delete…</MenuItem>
                                    </MenuContent>
                                </Menu>
                            </div>
                            {task?.id === id && form}
                        </li>
                    );
                })}
            </ul>
        </section>
    );
}
