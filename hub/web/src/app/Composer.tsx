import { useSkills } from './useSkills.ts';
import { skillCompletion } from './skillInput.ts';
import type { FailedInput } from '../state/store.ts';
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
 * on the fixed-width trigger whenever it is not the default — because a setting that
 * turns approvals off should not be discoverable only by opening the thing that
 * sets it.
 *
 * The other two carried-over behaviours: the draft survives a refused send
 * (defect D19, fixed in the store), and Enter sends while Shift+Enter breaks the
 * line. Command mode is explicit and keeps a separate query, so a command can
 * never be mistaken for a worker message or erase an unfinished draft.
 */
import { useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react';
import type { ContentPart, PayloadOptions } from '../../../ui/index.ts';
import { usePanel, useSession, useView } from '../state/usePanel.ts';
import { Button } from '../ui/Button.tsx';
import { Glyph } from '../ui/icons.tsx';
import { Popover, PopoverContent, PopoverTrigger } from '../ui/overlays.tsx';
import type { ConfirmMode } from '../state/store.ts';
import { useClient } from './ClientContext.tsx';
import { currentModelOptions, modelOptionFields } from './modelOptions.ts';
import { TokenUsageIndicator, TokenUsageMeter } from './TokenUsageIndicator.tsx';
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
    /** Kept so a refused reference is restored with the category it had. */
    readonly modality: ContentPart['modality'];
}

/** How a confirmation mode reads, and how loudly. */
const MODES: Record<ConfirmMode, { detail: string }> = {
    ask: {
        detail: 'every call that needs approval opens a prompt',
    },
    approve: {
        detail: 'every call that would have asked is approved without a prompt',
    },
    deny: {
        detail: 'every call that would have asked is denied without a prompt',
    },
};

/** The primary action occupies the same slot in both composer modes. */
const PRIMARY_ACTION_CLASS = 'h-9 w-32 shrink-0 justify-center whitespace-nowrap leading-5 max-sm:w-28 max-sm:px-1! max-sm:text-xs!';
const DEFAULT_INPUT_HEIGHT = 96;

export function Composer() {
    const selected = usePanel(state => state.selected);
    return <SessionComposer key={selected ?? ''} />;
}

/** Remounting on session change isolates asynchronous draft/skill work. */
function SessionComposer() {
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
    const skill = useSkills(draft, selected, client.rest);
    const [skillQuery, setSkillQuery] = useState('');
    const [caret, setCaret] = useState(0);
    const [completionHidden, setCompletionHidden] = useState(false);
    const [activeSkill, setActiveSkill] = useState(0);
    const [deferredFailure, setDeferredFailure] = useState<FailedInput | null>(null);
    const completion = entryMode === 'message' && !completionHidden ? skillCompletion(draft, caret) : null;
    const suggestions = skill.catalog.skills.filter(item => item.id.startsWith(completion?.query ?? ''));
    const suggested = suggestions[Math.min(activeSkill, Math.max(0, suggestions.length - 1))];
    const [confirmOpen, setConfirmOpen] = useState(false);
    const [modelOpen, setModelOpen] = useState(false);

    // A mode change closes settings rather than leaving a disabled trigger's
    // popover interactive above the shared toolbar.
    useEffect(() => {
        skill.setLibraryOpen(false);
        setConfirmOpen(false);
        setModelOpen(false);
    }, [entryMode, selected]);
    const box = useRef<HTMLTextAreaElement>(null);

    // A mode change retains the measured height and each mode's selection.
    // Editing grows the surface up to its scrolling limit. A new session or
    // successful message submission resets the now-empty surface.
    const inputHeight = useRef(DEFAULT_INPUT_HEIGHT);
    const selection = useRef({ message: [0, 0], command: [0, 0] });
    const resetGeometry = useCallback(() => {
        inputHeight.current = DEFAULT_INPUT_HEIGHT;
        selection.current = { message: [0, 0], command: [0, 0] };
        if (box.current) {
            box.current.style.height = `${DEFAULT_INPUT_HEIGHT}px`;
            box.current.setSelectionRange(0, 0);
        }
    }, []);
    useLayoutEffect(() => {
        const input = box.current;
        if (!input) return;
        input.style.height = `${inputHeight.current}px`;
        const [start, end] = selection.current[entryMode];
        input.setSelectionRange(start ?? 0, end ?? 0);
    }, [entryMode]);

    // Capture Alt+Enter before the textarea's Enter-to-send handler. Dialogs
    // own their own input and do not change the composer mode.
    useEffect(() => {
        function onKeyDown(event: KeyboardEvent): void {
            if (event.isComposing || event.defaultPrevented || event.repeat
                || !event.altKey || event.ctrlKey || event.metaKey
                || event.shiftKey || event.key !== 'Enter') return;
            if (event.target instanceof Element
                && event.target.closest('[role="dialog"]')) return;
            event.preventDefault();
            event.stopPropagation();
            if (box.current) {
                selection.current[entryMode] = [box.current.selectionStart, box.current.selectionEnd];
            }
            setEntryMode(entryMode === 'message' ? 'command' : 'message');
            box.current?.focus();
        }
        document.addEventListener('keydown', onKeyDown, true);
        return () => document.removeEventListener('keydown', onKeyDown, true);
    }, [entryMode]);

    // Switching sessions must not carry one session's draft, or its references,
    // into another.
    useLayoutEffect(() => {
        setDraft('');
        setCommandQuery('');
        setEntryMode('message');
        setReferences([]);
        skill.setLibraryOpen(false);
        resetGeometry();
    }, [selected, resetGeometry]);

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
        const original = failed.recovery?.draft ?? failed.parts.filter(part => part.type === 'text')
            .map(part => part.raw).join('\n\n');
        if (draft && draft !== original) {
            setDeferredFailure(failed);
            clearFailedInput();
            return;
        }
        setDraft(original);
        if (failed.recovery) skill.restore(failed.recovery.skills);
        setReferences(failed.parts.filter((part) => part.type === 'external_ref')
            .map((part) => ({
                kind: 'external_ref', raw: part.raw, modality: part.modality ?? 'text',
            })));
        clearFailedInput();
        box.current?.focus();
    }, [failed, selected, clearFailedInput]);

    if (!selected || !session) return null;

    const sessionId: string = selected;
    const connected = session.connected;
    const tokenUsage = view?.tokenUsage?.workerId === session.identity.worker_id
        ? view.tokenUsage : null;
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

    function insertSkill(id: string): void {
        const input = box.current;
        const start = completion?.start ?? input?.selectionStart ?? draft.length;
        const end = input?.selectionEnd ?? start;
        const marker = `@{skill:${id}}`;
        setDraft(draft.slice(0, start) + marker + draft.slice(end));
        skill.setLibraryOpen(false);
        setCompletionHidden(true);
        const nextCaret = start + marker.length;
        requestAnimationFrame(() => {
            input?.focus();
            input?.setSelectionRange(nextCaret, nextCaret);
            setCaret(nextCaret);
        });
    }

    function parts(): ContentPart[] {
        const list: ContentPart[] = [];
        if (draft.trim().length > 0) list.push({ type: 'text', raw: skill.expanded, modality: 'text' });
        for (const reference of references) {
            // Preserve references restored from older rejected payloads.
            list.push({
                type: 'external_ref', raw: reference.raw, modality: reference.modality,
            });
        }
        return list;
    }

    function send(operation: 'message' | 'continue' | 'compact' = 'message'): void {
        if (!connected || runActive || (operation === 'message' && (!canSend || skill.error))) return;
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
            operation === 'message' && (draft.includes('@{skill:'))
                ? { draft, skills: skill.skills } : undefined,
        );
        // Clear only when the frame actually left; otherwise the store has
        // already handed the text back and clearing here would lose it.
        if (sent && operation === 'message') {
            setDraft('');
            setReferences([]);
            // An empty, submitted draft gives its reading space back. Mode
            // changes and ongoing execution updates retain the current size.
            resetGeometry();
        }
    }

    return (
        <form
            className="composer-shell" data-testid="composer"
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
            <div className="h-5">{tokenUsage && <TokenUsageIndicator usage={tokenUsage} />}</div>
            <div className="reading-width composer-surface" data-testid="composer-surface">
                <div className="flex items-center px-2 pt-2 pb-1">
                    <span key={entryMode} data-testid="composer-mode"
                        className={`animate-enter w-28 shrink-0 whitespace-nowrap rounded px-1 py-1 text-center text-xs font-medium capitalize
                            ${entryMode === 'message' ? 'bg-info-soft text-info' : 'bg-warn-soft text-warn'}`}>
                        {entryMode} mode
                    </span>
                    <div className="ml-2">
                        {tokenUsage && <TokenUsageMeter usage={tokenUsage} />}
                    </div>
                    <span className="ml-auto hidden text-xs text-ink-faint sm:inline">Alt + Enter to switch</span>
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
                    aria-autocomplete={entryMode === 'command' || completion ? 'list' : undefined}
                    aria-controls={entryMode === 'command' ? 'composer-command-list' : completion ? 'composer-skill-list' : undefined}
                    aria-activedescendant={entryMode === 'command' && commands[highlighted]
                        ? `composer-command-${commands[highlighted].id}`
                        : completion && suggested ? `skill-option-${suggested.id}` : undefined}
                    onSelect={event => setCaret(event.currentTarget.selectionStart)}
                    onChange={(event) => {
                        const input = event.currentTarget;
                        input.style.height = 'auto';
                        inputHeight.current = Math.max(entryMode === 'command' ? inputHeight.current : DEFAULT_INPUT_HEIGHT,
                            Math.min(240, input.scrollHeight));
                        input.style.height = `${inputHeight.current}px`;
                        if (entryMode === 'message') { setDraft(event.target.value); setCaret(input.selectionStart); setCompletionHidden(false); setActiveSkill(0); }
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
                        if (completion) {
                            if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
                                event.preventDefault();
                                setActiveSkill(value => Math.max(0, Math.min(suggestions.length - 1,
                                    value + (event.key === 'ArrowDown' ? 1 : -1))));
                                return;
                            }
                            if ((event.key === 'Enter' || event.key === 'Tab') && suggested) {
                                event.preventDefault();
                                insertSkill(suggested.id);
                                return;
                            }
                            if (event.key === 'Escape') {
                                event.preventDefault();
                                setCompletionHidden(true);
                                return;
                            }
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
                    className="composer-input block h-24 min-h-24 max-h-60 w-full resize-none overflow-y-auto bg-transparent
                        px-3 pb-1 pt-2
                        text-sm leading-5 text-ink placeholder:text-ink-faint focus:outline-none"
                />

                {completion && (
                    <div id="composer-skill-list" role="listbox" aria-label="matching skills"
                        className="absolute bottom-full left-0 z-20 mb-1 max-h-60 w-full overflow-y-auto rounded-lg border border-line bg-raised p-1 shadow-lg">
                        {suggestions.map(item => <button key={item.id} type="button" role="option"
                            id={`skill-option-${item.id}`} aria-selected={item.id === suggested?.id}
                            onMouseDown={event => event.preventDefault()} onClick={() => insertSkill(item.id)}
                            className={`block w-full rounded p-2 text-left text-xs ${item.id === suggested?.id ? 'bg-subtle' : ''}`}>
                            {item.title} — {item.id}
                        </button>)}
                        {!suggestions.length && <p className="p-2 text-xs">No matching skills.</p>}
                    </div>
                )}
                {draft.includes('@{skill:') && (
                    <div className="px-3 pb-1 text-xs text-ink-muted">
                        {skill.error ? <p role="status" className="text-warn">{skill.error}</p>
                            : <span>{skill.ids.length} skill(s) ready · inserted when sending</span>}
                        {skill.ids.some(id => skill.catalog.skills.some(item => item.id === id
                            && skill.skills[id] && item.revision !== skill.skills[id]?.revision))
                            && <p className="text-warn">Skill file changed. The selected snapshot is retained until refreshed.</p>}
                        <button type="button" className="ml-2 underline" disabled={entryMode === 'command'} onClick={skill.refreshSnapshots}>Refresh skill text</button>
                        {!skill.error && <details><summary className="cursor-pointer">Preview message</summary>
                            <pre className="max-h-48 overflow-auto whitespace-pre-wrap break-words [overflow-wrap:anywhere] p-2">{skill.expanded}</pre>
                        </details>}
                    </div>
                )}
                {deferredFailure && <div role="status" className="px-3 text-xs text-warn">
                    A previous message was rejected. Your current draft has been kept.
                    <button type="button" className="ml-2 underline" onClick={() => {
                        setDraft(deferredFailure.recovery?.draft ?? deferredFailure.parts.filter(p => p.type === 'text').map(p => p.raw).join('\n\n'));
                        skill.restore(deferredFailure.recovery?.skills ?? {});
                        setReferences(deferredFailure.parts.filter(p => p.type === 'external_ref').map(p => ({ kind: 'external_ref', raw: p.raw, modality: p.modality ?? 'text' })));
                        setDeferredFailure(null);
                    }}>Restore rejected draft</button>
                    <button type="button" className="ml-2 underline" onClick={() => setDeferredFailure(null)}>Dismiss</button>
                </div>}

                {entryMode === 'command' && (
                    <div id="composer-command-list" role="listbox"
                        className="absolute bottom-full left-0 z-20 mb-1 max-h-[min(16rem,35dvh)] w-full
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

                <div className="flex items-center gap-1 px-2 pb-2 sm:gap-2">
                    <Popover open={skill.libraryOpen} onOpenChange={skill.setLibraryOpen}>
                        <PopoverTrigger asChild>
                            <Button aria-label="Choose a skill" variant="ghost" size="md"
                                disabled={entryMode === 'command'}
                                className="h-9 justify-center max-sm:px-1! max-sm:text-xs! leading-5"
                                icon={<Glyph name="attach" />}>
                                <span className="hidden min-[380px]:inline">Skills</span>
                            </Button>
                        </PopoverTrigger>
                        <PopoverContent align="start" width="w-80">
                            <label className="text-xs text-ink-muted">Search skills
                                <input aria-label="Search skills" value={skillQuery}
                                    onChange={event => setSkillQuery(event.target.value)}
                                    className="mt-1 w-full rounded border border-line bg-surface p-2 text-sm focus:outline-none" />
                            </label>
                            <p className="my-2 text-xs text-ink-muted">Insert at the cursor. Edit or delete the marker in your message.</p>
                            <div className="max-h-60 overflow-y-auto">
                                {skill.catalog.skills.filter(item =>
                                    `${item.id} ${item.title} ${item.description} ${item.keywords.join(' ')}`
                                        .toLowerCase().includes(skillQuery.toLowerCase())).map(item => (
                                    <button type="button" key={item.id} onClick={() => insertSkill(item.id)}
                                        className="block w-full rounded p-2 text-left hover:bg-subtle focus-visible:outline-none focus-visible:bg-subtle">
                                        <span className="block text-sm font-medium">{item.title}</span>
                                        <span className="block text-xs text-ink-muted">{item.description}</span>
                                        <span className="font-mono text-xs text-ink-faint">{`@{skill:${item.id}}`}</span>
                                    </button>
                                ))}
                                {skill.catalog.skills.length === 0 && <p className="text-xs">No skills available.</p>}
                                {skill.catalog.errors.map(item => <p key={item.id} className="text-xs text-warn">{item.id}: {item.message}</p>)}
                            </div>
                            {skill.libraryError && <p role="alert" className="text-xs text-danger">{skill.libraryError}</p>}
                            <Button type="button" variant="ghost" size="sm" onClick={skill.reload}>Refresh library</Button>
                        </PopoverContent>
                    </Popover>

                    <Popover open={confirmOpen} onOpenChange={setConfirmOpen}>
                        <PopoverTrigger asChild>
                            <Button
                                aria-label={`confirmation mode: ${mode}`}
                                disabled={entryMode === 'command'}
                                variant="ghost"
                                size="md"
                                className="h-9 w-[72px] justify-center max-sm:px-1! max-sm:text-xs! leading-5 sm:w-24"
                                icon={<Glyph name="options" />}
                                title={MODES[mode].detail}
                            >
                                <span className={mode === 'approve' ? 'text-danger'
                                    : mode === 'deny' ? 'text-warn' : undefined}>
                                    {mode === 'ask' ? 'Confirm' : mode === 'approve' ? 'Approve' : 'Deny'}
                                </span>
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
                                title={cancelPending ? "Waiting for an interruptible boundary; tools may still finish." : "Ask the worker to cancel the active run"}
                                className={PRIMARY_ACTION_CLASS}
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
                                    : !connected || !canSend || Boolean(skill.error)}
                                className={PRIMARY_ACTION_CLASS}
                                icon={<Glyph name="send" />}
                            >
                                <span>Send</span>
                            </Button>
                        )}
                    </div>
                </div>
            </div>

            <div className="reading-width composer-footer mt-1 flex items-center gap-2
                px-1 text-xs text-ink-muted" aria-live="polite">
                {cancelPending && <span role="status" className="text-warn">
                    Cancellation requested. Waiting for an interruptible boundary.
                </span>}
                {connectionState !== 'open' && (
                    <span className="text-warn">the panel is not connected</span>
                )}
                {!connected && <span>no worker attached</span>}
                <span className="flex-1" />
                <span className="hidden sm:inline">{cancelPending ? '' : runActive ? 'Draft kept for your next request' : 'Enter to send'}</span>
            </div>
        </form>
    );
}
