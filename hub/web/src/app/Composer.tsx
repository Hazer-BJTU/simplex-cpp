/**
 * @file the message and command composer.
 *
 * The old one put every control on a single row — Send, Continue, a URL field,
 * "Add ref", a confirmation-mode `<select>`, and a hint — and let `flex-wrap`
 * sort it out. Worse, the confirmation mode was one DOM control shared by every
 * session and never reset, so choosing `approve` in one session silently applied
 * it to all of them (defect D15): `approve` means "approve every call that would
 * have asked", so the leak quietly disabled approvals session-wide.
 *
 * Here the mode belongs to a session, lives in a compact settings popover, and is shown
 * as a permanent badge whenever it is not the default — because a setting that
 * turns approvals off should not be discoverable only by opening the thing that
 * sets it.
 *
 * The other two carried-over behaviours: the draft survives a refused send
 * (defect D19, fixed in the store), and Enter sends while Shift+Enter breaks the
 * line. Command mode is explicit and keeps a separate query, so a command can
 * never be mistaken for a worker message or erase an unfinished draft.
 */
import { useEffect, useRef, useState } from 'react';
import type { ContentPart, PayloadOptions } from '../../../shared/protocol.ts';
import { usePanel, useSession, useView } from '../state/usePanel.ts';
import { Badge, Button } from '../ui/Button.tsx';
import { Glyph } from '../ui/icons.tsx';
import { Popover, PopoverContent, PopoverTrigger } from '../ui/overlays.tsx';
import type { ConfirmMode } from '../state/store.ts';
import { useClient } from './ClientContext.tsx';
import { currentModelOptions, modelOptionFields } from './modelOptions.ts';
import {
    matchingComposerCommands,
    compactRetentionDetail,
    unavailableReason,
    type ComposerCommand,
} from './composerCommands.ts';

/** One removable part the operator attached. */
interface Reference {
    readonly kind: 'external_ref';
    readonly raw: string;
}

/** How a confirmation mode reads, and how loudly. */
const MODES: Record<ConfirmMode, { label: string; detail: string; tone: 'neutral' | 'warn' | 'bad' }> = {
    ask: {
        label: 'ask',
        detail: 'every call that needs approval opens a prompt',
        tone: 'neutral',
    },
    approve: {
        label: 'approve',
        detail: 'every call that would have asked is approved without a prompt',
        tone: 'bad',
    },
    deny: {
        label: 'deny',
        detail: 'every call that would have asked is denied without a prompt',
        tone: 'warn',
    },
};

/** The primary action occupies the same slot in both composer modes. */
const PRIMARY_ACTION_CLASS = 'h-9 w-[76px] justify-center leading-5';

export function Composer() {
    const client = useClient();
    const selected = usePanel((state) => state.selected);
    const session = useSession(selected);
    const view = useView(selected);
    const failed = usePanel((state) => state.failedInput);
    const clearFailedInput = usePanel((state) => state.clearFailedInput);
    const connectionState = usePanel((state) => state.connection.state);
    const hubCompactSupported = usePanel((state) => state.hasCapability('context-compact'));
    const mode = usePanel((state) => (state.selected
        ? state.confirmMode.get(state.selected) ?? 'ask'
        : 'ask'));
    const setConfirmMode = usePanel((state) => state.setConfirmMode);
    const setModelOption = usePanel((state) => state.setModelOption);

    const [draft, setDraft] = useState('');
    const [entryMode, setEntryMode] = useState<'message' | 'command'>('message');
    const [commandQuery, setCommandQuery] = useState('');
    const [activeCommand, setActiveCommand] = useState(0);
    const [references, setReferences] = useState<readonly Reference[]>([]);
    const [refOpen, setRefOpen] = useState(false);
    const [confirmOpen, setConfirmOpen] = useState(false);
    const [modelOpen, setModelOpen] = useState(false);

    // A mode change closes settings rather than leaving a disabled trigger's
    // popover interactive above the shared toolbar.
    useEffect(() => {
        setRefOpen(false);
        setConfirmOpen(false);
        setModelOpen(false);
    }, [entryMode, selected]);
    const box = useRef<HTMLTextAreaElement>(null);

    // Capture Alt+Enter before the textarea's Enter-to-send handler. Dialogs
    // own their own input and do not change the composer mode.
    useEffect(() => {
        function onKeyDown(event: KeyboardEvent): void {
            if (event.defaultPrevented || event.repeat
                || !event.altKey || event.ctrlKey || event.metaKey
                || event.shiftKey || event.key !== 'Enter') return;
            if (event.target instanceof Element
                && event.target.closest('[role="dialog"]')) return;
            event.preventDefault();
            event.stopPropagation();
            setEntryMode((current) => current === 'message' ? 'command' : 'message');
            box.current?.focus();
        }
        document.addEventListener('keydown', onKeyDown, true);
        return () => document.removeEventListener('keydown', onKeyDown, true);
    }, []);

    // Switching sessions must not carry one session's draft, or its references,
    // into another.
    useEffect(() => {
        setDraft('');
        setCommandQuery('');
        setEntryMode('message');
        setReferences([]);
        setRefOpen(false);
    }, [selected]);

    // Restore each refused part in its original form. A reference must remain
    // an external_ref: turning its URL into message text silently changes the
    // next request. This runs after the selection reset so a failed request
    // for the newly selected session is not cleared by that reset.
    useEffect(() => {
        if (!failed || failed.sessionId !== selected) return;
        if (failed.operation === 'continue' || failed.operation === 'compact') {
            // A continuation has no content to refund. In particular, a
            // rejected continuation must not erase a separate unsent draft.
            clearFailedInput();
            return;
        }
        setDraft(failed.parts.filter((part) => part.type === 'text')
            .map((part) => part.raw).join('\n\n'));
        setReferences(failed.parts.filter((part) => part.type === 'external_ref')
            .map((part) => ({ kind: 'external_ref', raw: part.raw })));
        clearFailedInput();
        box.current?.focus();
    }, [failed, selected, clearFailedInput]);

    if (!selected || !session) return null;

    const sessionId: string = selected;
    const connected = session.connected;
    const optionsEvent = view?.modelCatalog;
    const modelData = optionsEvent?.worker_id === session.identity.worker_id
        ? optionsEvent?.data : undefined;
    const modelFields = modelOptionFields(modelData);
    const modelChoices = { ...currentModelOptions(modelData), ...view?.modelSelection };
    const runActive = Boolean(view?.runActive);
    const cancelPending = runActive && Boolean(view?.cancelPending);
    const compactSupported = hubCompactSupported
        && Boolean(session.worker_capabilities?.includes('context-compact'));
    const latestStatus = view?.latestEvents.status;
    const latestReady = view?.latestEvents.ready;
    const workerStatus = (latestStatus?.worker_id === session.identity.worker_id
        ? latestStatus : latestReady?.worker_id === session.identity.worker_id
            ? latestReady : undefined)?.data as
        Record<string, unknown> | undefined;
    const canSend = draft.trim().length > 0 || references.length > 0;
    const commands = matchingComposerCommands(commandQuery);
    const highlighted = Math.min(activeCommand, Math.max(0, commands.length - 1));
    const selectedCommand = commands[highlighted];
    const selectedUnavailable = selectedCommand
        ? unavailableReason(selectedCommand, connected, runActive, compactSupported)
        : null;

    function runCommand(command: ComposerCommand): void {
        if (unavailableReason(command, connected, runActive, compactSupported)) return;
        if (command.id === 'refresh-conversation') {
            client.refreshConversation(sessionId);
        } else if (command.id === 'continue-run') {
            send('continue');
        } else if (command.id === 'compact-context') {
            send('compact');
        }
        setCommandQuery('');
        setActiveCommand(0);
        box.current?.focus();
    }

    function parts(): ContentPart[] {
        const list: ContentPart[] = [];
        if (draft.trim().length > 0) list.push({ type: 'text', raw: draft });
        for (const reference of references) {
            list.push({ type: 'external_ref', raw: reference.raw });
        }
        return list;
    }

    function send(operation: 'message' | 'continue' | 'compact' = 'message'): void {
        if (!connected || runActive || (operation === 'message' && !canSend)) return;
        // The mode travels with the payload. The worker freezes the policy per
        // run, so sending it every time is what makes a change take effect on
        // the next run rather than the next restart — and what keeps one
        // session's choice out of another's.
        const options: PayloadOptions = { confirmation: { mode } };
        if (modelData && view && Object.keys(view.modelSelection).length > 0) {
            options.model = { ...view.modelSelection };
        }
        const sent = client.sendInput(
            sessionId,
            operation === 'message' ? parts() : [],
            operation,
            options,
        );
        // Clear only when the frame actually left; otherwise the store has
        // already handed the text back and clearing here would lose it.
        if (sent && operation === 'message') {
            setDraft('');
            setReferences([]);
        }
    }

    return (
        <form
            className="shrink-0 border-t border-line bg-surface px-3 py-2 sm:px-5"
            onSubmit={(event) => {
                event.preventDefault();
                if (entryMode === 'command') {
                    const command = commands[highlighted];
                    if (command) runCommand(command);
                } else {
                    send();
                }
            }}
        >
            <div className="relative mx-auto max-w-4xl rounded-xl border border-line-strong
                bg-surface shadow-sm focus-within:border-ink-muted">
                <div className="flex items-center border-b border-line px-2 py-1.5">
                    <span key={entryMode} data-testid="composer-mode"
                        className={`animate-enter rounded px-2 py-1 text-xs font-medium capitalize
                            ${entryMode === 'message' ? 'bg-info-soft text-info' : 'bg-warn-soft text-warn'}`}>
                        {entryMode} mode
                    </span>
                    <span className="ml-auto text-xs text-ink-faint">Alt + Enter to switch</span>
                </div>
                {references.length > 0 && (
                    <ul className="flex flex-wrap gap-1.5 px-4 pt-3">
                        {references.map((reference) => (
                            <li
                                key={reference.raw}
                                className="flex items-center gap-1 rounded-full border border-line
                                    bg-subtle py-1 pl-2.5 pr-1 text-xs text-ink-muted"
                            >
                                <span className="max-w-72 truncate font-mono">{reference.raw}</span>
                                <button
                                    type="button"
                                    aria-label={`remove reference ${reference.raw}`}
                                    disabled={entryMode === 'command'}
                                    className="rounded-full p-0.5 hover:bg-line"
                                    onClick={() => setReferences((current) => (
                                        current.filter((item) => item.raw !== reference.raw)
                                    ))}
                                >
                                    <Glyph name="close" size="sm" />
                                </button>
                            </li>
                        ))}
                    </ul>
                )}

                <textarea
                    ref={box}
                    value={entryMode === 'message' ? draft : commandQuery}
                    rows={3}
                    aria-label={entryMode === 'message' ? 'message' : 'command input'}
                    aria-autocomplete={entryMode === 'command' ? 'list' : undefined}
                    aria-controls={entryMode === 'command' ? 'composer-command-list' : undefined}
                    aria-activedescendant={entryMode === 'command' && commands[highlighted]
                        ? `composer-command-${commands[highlighted].id}`
                        : undefined}
                    onChange={(event) => {
                        if (entryMode === 'message') setDraft(event.target.value);
                        else { setCommandQuery(event.target.value); setActiveCommand(0); }
                    }}
                    onKeyDown={(event) => {
                        if (event.nativeEvent.isComposing) return;
                        if (entryMode === 'command') {
                            if (event.key === 'ArrowDown' && commands.length > 0) {
                                event.preventDefault();
                                setActiveCommand((current) => Math.min(current + 1, commands.length - 1));
                            } else if (event.key === 'ArrowUp' && commands.length > 0) {
                                event.preventDefault();
                                setActiveCommand((current) => Math.max(current - 1, 0));
                            } else if (event.key === 'Tab' && commands[highlighted]) {
                                event.preventDefault();
                                setCommandQuery(commands[highlighted].name);
                            } else if (event.key === 'Enter') {
                                event.preventDefault();
                                const command = commands[highlighted];
                                if (command) runCommand(command);
                            } else if (event.key === 'Escape') {
                                event.preventDefault();
                                setCommandQuery('');
                            }
                            return;
                        }
                        if (event.key === 'Enter' && !event.shiftKey) {
                            event.preventDefault();
                            send();
                            return;
                        }
                        if (event.key === 'Escape') {
                            event.preventDefault();
                            setDraft('');
                        }
                    }}
                    placeholder={entryMode === 'command'
                        ? 'Type a command name…'
                        : connected ? 'Message the worker…' : 'No worker is attached to this session'}
                    className="composer-input block h-24 w-full resize-none overflow-y-auto bg-transparent
                        px-3 pb-1 pt-2
                        text-sm leading-5 text-ink placeholder:text-ink-faint focus:outline-none"
                />

                {entryMode === 'command' && (
                    <div id="composer-command-list" role="listbox"
                        className="absolute bottom-full left-0 z-20 mb-1 max-h-64 w-full
                            overflow-y-auto rounded-lg border border-line-strong bg-raised
                            p-1 shadow-lg"
                        aria-label="matching commands">
                        {commands.length === 0 && (
                            <p className="px-2 py-2 text-xs text-ink-muted">
                                No command starts with “{commandQuery}”.
                            </p>
                        )}
                        {commands.map((command, index) => {
                            const reason = unavailableReason(command, connected, runActive, compactSupported);
                            return (
                                <button key={command.id} id={`composer-command-${command.id}`}
                                    type="button" role="option"
                                    aria-selected={index === highlighted}
                                    disabled={reason !== null}
                                    onMouseEnter={() => setActiveCommand(index)}
                                    onClick={() => runCommand(command)}
                                    className={`block w-full rounded px-2 py-2 text-left text-xs
                                        disabled:cursor-not-allowed
                                        ${index === highlighted ? 'bg-subtle' : 'hover:bg-subtle'}`}>
                                    <span className="block font-medium text-ink">{command.name}</span>
                                    <span className="block text-ink-muted">{command.detail}</span>
                                    {command.id === 'compact-context' && (
                                        <span className="block text-ink-faint">
                                            {compactRetentionDetail(workerStatus?.memory_retention)}
                                        </span>
                                    )}
                                    {reason && <span className="block text-warn">{reason}</span>}
                                </button>
                            );
                        })}
                    </div>
                )}

                <div className="flex flex-wrap items-center gap-1 px-2 pb-2 sm:gap-2">
                    <Popover open={refOpen} onOpenChange={setRefOpen}>
                        <PopoverTrigger asChild>
                            <Button
                                aria-label="Attach a reference"
                                variant="ghost"
                                size="md"
                                // Attachment entry is reserved until the feature is ready.
                                disabled
                                className="h-9 justify-center max-sm:px-1! max-sm:text-xs! leading-5"
                                icon={<Glyph name="attach" />}
                            >
                                <span>Attach</span>
                            </Button>
                        </PopoverTrigger>
                        <PopoverContent align="start" width="w-80">
                            <form
                                onSubmit={(event) => {
                                    event.preventDefault();
                                    const field = event.currentTarget.elements.namedItem('reference');
                                    if (!(field instanceof HTMLInputElement)) return;
                                    const value = field.value.trim();
                                    if (!value) return;
                                    // A reference is sent as data, not fetched by the panel.
                                    setReferences((current) => (
                                        current.some((item) => item.raw === value)
                                            ? current
                                            : [...current, { kind: 'external_ref', raw: value }]
                                    ));
                                    field.value = '';
                                    setRefOpen(false);
                                }}
                            >
                                <label className="block text-xs font-medium text-ink-muted"
                                    htmlFor="composer-reference">
                                    external reference
                                </label>
                                <input
                                    id="composer-reference"
                                    name="reference"
                                    autoFocus
                                    placeholder="https://…"
                                    className="mt-1 w-full rounded border border-line-strong px-2 py-1
                                        font-mono text-xs focus:border-line-strong focus:outline-none"
                                />
                                <p className="mt-1 text-xs text-ink-muted">
                                    Sent to the worker as an <code>external_ref</code> part. The panel
                                    never fetches it.
                                </p>
                                <div className="mt-2 flex justify-end">
                                    <Button type="submit" variant="primary">Attach</Button>
                                </div>
                            </form>
                        </PopoverContent>
                    </Popover>

                    <Popover open={confirmOpen} onOpenChange={setConfirmOpen}>
                        <PopoverTrigger asChild>
                            <Button
                                aria-label={`confirmation mode: ${mode}`}
                                disabled={entryMode === 'command'}
                                variant="ghost"
                                size="md"
                                className="h-9 justify-center max-sm:px-1! max-sm:text-xs! leading-5"
                                icon={<Glyph name="options" />}
                                title="how the worker should answer tool confirmations for this session"
                            >
                                <span>Confirm</span>
                            </Button>
                        </PopoverTrigger>
                        <PopoverContent align="start">
                            <p className="text-xs font-medium text-ink">
                                Confirmation mode for <span className="font-mono">{sessionId}</span>
                            </p>
                            <div className="mt-2 space-y-1">
                                {(['ask', 'approve', 'deny'] as const).map((value) => (
                                    <label
                                        key={value}
                                        className="flex cursor-pointer items-start gap-2 rounded p-1
                                            hover:bg-sunken"
                                    >
                                        <input
                                            type="radio"
                                            name="confirm-mode"
                                            value={value}
                                            checked={mode === value}
                                            onChange={() => setConfirmMode(sessionId, value)}
                                            className="mt-0.5 accent-interactive"
                                        />
                                        <span>
                                            <span className="font-mono text-xs text-ink">{value}</span>
                                            <span className="block text-xs text-ink-muted">
                                                {MODES[value].detail}
                                            </span>
                                        </span>
                                    </label>
                                ))}
                            </div>
                            <p className="mt-2 border-t border-line pt-2 text-xs text-ink-muted">
                                This choice applies to the next run for this session.
                            </p>
                        </PopoverContent>
                    </Popover>

                    <Popover open={modelOpen} onOpenChange={setModelOpen}>
                        <PopoverTrigger asChild>
                            <Button aria-label="Model options" variant="ghost" size="md"
                                disabled={entryMode === 'command' || !connected || modelFields.length === 0}
                                className="h-9 justify-center max-sm:px-1! max-sm:text-xs! leading-5"
                                icon={<span className="hidden sm:inline-flex"><Glyph name="options" /></span>}>
                                <span>Model</span>
                            </Button>
                        </PopoverTrigger>
                        <PopoverContent align="start" width="w-80">
                            <p className="text-xs font-medium text-ink">Model options</p>
                            <div className="mt-2 space-y-3">
                                {modelFields.map((field) => {
                                    const current = modelChoices[field.name];
                                    const index = field.options.findIndex((value) =>
                                        JSON.stringify(value) === JSON.stringify(current));
                                    return (
                                        <label key={field.name} className="block text-xs text-ink-muted">
                                            {field.name}
                                            <select aria-label={`Model option: ${field.name}`}
                                                className="mt-1 block w-full rounded border border-line bg-surface p-2 text-ink"
                                                value={index < 0 ? '' : String(index)}
                                                onChange={(event) => setModelOption(sessionId,
                                                    field.name, field.options[Number(event.target.value)])}>
                                                {index < 0 && <option value="" disabled>
                                                    {current === undefined ? 'Select an option' : String(current)}
                                                </option>}
                                                {field.options.map((value, optionIndex) => (
                                                    <option key={optionIndex} value={optionIndex}>
                                                        {typeof value === 'string' ? value : JSON.stringify(value)}
                                                    </option>
                                                ))}
                                            </select>
                                        </label>
                                    );
                                })}
                            </div>
                            <p className="mt-3 text-xs text-ink-muted">
                                These choices apply to the next run, including Continue run and Compact context.
                            </p>
                        </PopoverContent>
                    </Popover>

                    <div className="ml-auto flex items-center gap-2">
                        {runActive ? (
                            <Button
                                variant="danger"
                                size="md"
                                disabled={!connected || cancelPending}
                                aria-busy={cancelPending}
                                onClick={() => client.sendSignal(sessionId, 'cancel')}
                                title="ask the worker to cancel the active run"
                                className="h-9 justify-center leading-5"
                                icon={<Glyph name={cancelPending ? 'spinner' : 'cancel'} />}
                            >
                                <span>{cancelPending ? 'Cancelling…' : 'Cancel run'}</span>
                            </Button>
                        ) : (
                            <Button
                                type="submit"
                                variant="primary"
                                size="md"
                                disabled={entryMode === 'command'
                                    ? !selectedCommand || selectedUnavailable !== null
                                    : !connected || !canSend}
                                className={PRIMARY_ACTION_CLASS}
                                icon={<Glyph name="send" />}
                            >
                                <span>Send</span>
                            </Button>
                        )}
                    </div>
                </div>
            </div>

            {cancelPending && (
                <p role="status" className="mx-auto mt-2 max-w-4xl px-1 text-xs text-warn">
                    Cancellation requested. Waiting for the worker to reach an interruptible boundary.
                </p>
            )}

            <div className="mx-auto mt-1 flex max-w-4xl flex-wrap items-center gap-x-3 gap-y-1
                px-1 text-xs text-ink-muted">
                {connectionState !== 'open' && (
                    <span className="text-warn">the panel is not connected</span>
                )}
                {!connected && <span>no worker attached</span>}
                {mode !== 'ask' && (
                    <Badge tone={MODES[mode].tone} title={MODES[mode].detail}>
                        approvals: {MODES[mode].label}
                    </Badge>
                )}

                <span className="flex-1" />
                <span className="hidden sm:inline">{runActive
                    ? 'Cancel stops this run · draft stays here'
                    : 'Enter to send'}</span>
            </div>
        </form>
    );
}
