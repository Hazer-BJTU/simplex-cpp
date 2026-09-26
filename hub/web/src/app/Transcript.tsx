/**
 * @file the conversation.
 *
 * The old panel appended one card per event to a growing list, at one visual
 * weight, and never removed anything. This renders the same transcript through
 * the two decisions that were missing:
 *
 * - **A turn is a unit.** `rounds.ts` decides what belongs to what, and this
 *   draws the result: the operator's message, the model's replies with the
 *   tools they asked for, and the machinery folded away.
 * - **Only the recent turns are open.** A long session collapses its earlier
 *   rounds to a one-line summary that can be opened, which is what keeps a
 *   thousand events readable without a virtual list and without the DOM growing
 *   without bound (defect D22).
 *
 * Scrolling follows the end only when the reader is already there (defect D24),
 * which the previous stage established and this one keeps.
 */
import {
    useCallback,
    useEffect,
    useLayoutEffect,
    useMemo,
    useRef,
    useState,
} from 'react';
import type {
    ContentPart,
    ConfirmationPrompt,
    HistoryTurn,
    RequestRecord,
    WorkerEnvelope,
} from '../../../shared/protocol.ts';
import { usePanel, useSession, useView } from '../state/usePanel.ts';
import { statsOf, type NoteItem, type OutboxItem, type TranscriptItem } from '../state/view.ts';
import { useClient } from './ClientContext.tsx';
import { EmptyState, LoadingLines } from '../ui/States.tsx';
import { Glyph } from '../ui/icons.tsx';
import { Markdown } from './Markdown.tsx';
import { ToolCard } from './ToolCard.tsx';
import { clockOf, contentText, formatDuration, prettyJson, str } from './content.ts';
import {
    buildRounds,
    type AssistantBlock,
    type Problem,
    type Round,
    type RunFailure,
    type ToolCall,
} from './rounds.ts';

/** How close to the bottom still counts as "following the end". */
const STICK_THRESHOLD_PX = 48;

/** How many of the most recent turns stay open. */
const OPEN_ROUNDS = 3;

/** Length at which an operator's own message is folded. */
const LONG_MESSAGE_CHARS = 600;

const EMPTY_ITEMS: readonly TranscriptItem[] = [];
const EMPTY_PROMPTS: ReadonlyMap<string, ConfirmationPrompt> = new Map();
const EMPTY_REQUESTS: ReadonlyMap<string, RequestRecord> = new Map();

/**
 * Describe the last visible phase of an active run. The worker sends complete
 * model responses rather than token chunks, so this is an activity cue, not a
 * claim that text is streaming or that thinking and generation are separable.
 */
function activityLabel(items: readonly TranscriptItem[]): string {
    for (let index = items.length - 1; index >= 0; index -= 1) {
        const item = items[index];
        if (item?.kind !== 'event') continue;
        switch (item.envelope.event) {
            case 'input_admitted':
                return 'Starting run';
            case 'tool_calls':
                return 'Running tools';
            case 'model_response':
                return 'Processing response';
            case 'run_started':
            case 'input_committed':
            case 'tool_results':
            case 'persisted':
                return 'Waiting for model response';
            default:
                break;
        }
    }
    return 'Working on this run';
}

/** A quiet, accessible activity cue at the end of the live conversation. */
function RunActivity({ label }: { label: string }) {
    return (
        <div
            data-testid="run-activity"
            role="status"
            aria-live="polite"
            className="animate-enter flex w-fit items-center gap-3 rounded-xl border border-line
                bg-sunken px-3 py-2 text-sm text-ink-muted"
        >
            <span aria-hidden="true" className="flex items-center gap-1">
                <span className="activity-dot" />
                <span className="activity-dot" />
                <span className="activity-dot" />
            </span>
            <span>
                <span className="block">{label}</span>
                {label === 'Waiting for model response' && (
                    <span className="block text-xs text-ink-faint">Reply appears when complete</span>
                )}
            </span>
        </div>
    );
}

/**
 * One line of the quiet protocol timeline.
 *
 * Rendering this at all is the "technical details" switch. The raw payload is
 * one click further in, because a fold that is open by default is not a fold —
 * and the old panel had one hanging off nearly every card.
 */
function ProtocolLine({ envelope }: { envelope: WorkerEnvelope }) {
    const label = str(envelope.event) || '(unnamed event)';
    const clock = clockOf(envelope);
    return (
        <div data-testid="protocol-line" className="text-xs text-ink-faint">
            <div className="flex items-baseline gap-2">
                <span className="h-px flex-1 bg-line" />
                <span className="font-mono">{label}</span>
                {clock && <span>{clock}</span>}
                <span className="h-px flex-1 bg-line" />
            </div>
            <details className="mt-0.5 text-center">
                <summary className="cursor-pointer select-none">payload</summary>
                <pre className="mt-1 max-h-64 overflow-auto whitespace-pre-wrap break-all rounded
                    bg-sunken p-2 text-left font-mono text-xs text-ink">
                    {prettyJson(envelope.raw ?? envelope.data ?? {})}
                </pre>
            </details>
        </div>
    );
}

/** A note the panel itself added: a replay gap, a restart, a warning. */
function NoteLine({ item }: { item: NoteItem }) {
    const tone = item.tone === 'error'
        ? 'border-danger-line bg-danger-soft text-danger'
        : item.tone === 'warn'
            ? 'border-warn-line bg-warn-soft text-warn'
            : 'border-line bg-sunken text-ink-muted';
    return (
        <div data-testid="transcript-note" className={`rounded border px-2 py-1 text-xs ${tone}`}>
            {item.text}
        </div>
    );
}

/** A problem the worker reported. */
function ProblemLine({ problem }: { problem: Problem }) {
    const tone = problem.tone === 'error'
        ? 'border-danger-line bg-danger-soft text-danger'
        : 'border-warn-line bg-warn-soft text-warn';
    return (
        <div data-testid="transcript-problem" className={`rounded border-l-2 px-2 py-1 ${tone}`}>
            <p className="text-xs font-medium">{problem.label}</p>
            <p className="text-xs">{problem.text}</p>
        </div>
    );
}

/** Visible run failure; raw provider diagnostics stay available on demand. */
function RunFailureNotice({ failure }: { failure: RunFailure }) {
    const model = failure.stage === 'model_request';
    return (
        <div data-testid="run-failure" role="alert"
            className="rounded-lg border border-danger-line bg-danger-soft px-3 py-2 text-sm
                text-danger">
            <p className="font-medium">{model ? 'Model request failed' : 'Run failed'}</p>
            <p className="mt-1">
                {model && failure.canContinue
                    ? 'The worker kept the conversation state. Use Continue run in Command mode to try again.'
                    : failure.canContinue
                        ? 'The worker kept the conversation state. Inspect the error before continuing.'
                        : 'Inspect the error and worker state before trying again.'}
            </p>
            {failure.error && (
                <details className="mt-2">
                    <summary className="cursor-pointer text-xs">Technical details</summary>
                    <pre className="mt-1 max-h-48 overflow-auto whitespace-pre-wrap break-all
                        rounded border border-danger-line p-2 text-xs">{failure.error}</pre>
                </details>
            )}
        </div>
    );
}

/** What the operator sent. */
function UserMessage({ item }: { item: OutboxItem }) {
    const [open, setOpen] = useState(false);
    const text = item.parts.map((part: ContentPart) => part.raw).join('\n\n');
    const long = text.length > LONG_MESSAGE_CHARS;
    const shown = long && !open ? `${text.slice(0, LONG_MESSAGE_CHARS)}…` : text;
    return (
        <article
            data-testid="outbox-item"
            data-state={item.state}
            className="ml-auto w-fit max-w-full sm:max-w-[80%] rounded-lg bg-accent px-3 py-2 text-accent-ink"
        >
            <p className="whitespace-pre-wrap break-words [overflow-wrap:anywhere] text-sm">{shown}</p>
            {long && (
                <button
                    type="button"
                    className="mt-1 text-xs text-ink-faint hover:text-ink-faint"
                    onClick={() => setOpen((value) => !value)}
                >
                    {open ? 'show less' : `show all ${text.length} characters`}
                </button>
            )}
            <p className="mt-1 text-xs text-ink-faint">
                {item.state === 'admitted' ? 'admitted by the worker' : 'sent, not yet admitted'}
            </p>
        </article>
    );
}

/** The placeholder for an input the worker carried but does not report. */
function AdmittedPlaceholder() {
    return (
        <p
            data-testid="admitted-placeholder"
            className="rounded border border-dashed border-line-strong px-2 py-1 text-xs
                italic text-ink-muted"
        >
            user input — the worker protocol reports that an input was admitted, not what it said
        </p>
    );
}

/** Recover the user's text for a detailed run replayed from hub events. */
function RestoredUserMessage({ turn }: { turn: HistoryTurn }) {
    const text = turn.user.map(contentText).filter(Boolean).join('\n\n');
    return (
        <article data-testid="restored-user-message" className="ml-auto w-fit max-w-full
            rounded-lg bg-accent px-3 py-2 text-accent-ink">
            <p className="whitespace-pre-wrap break-words [overflow-wrap:anywhere] text-sm">
                {text || '(empty input)'}
            </p>
        </article>
    );
}

/** Compact history projection; tool arguments and results never enter it. */
function HistoryRound({ turn, open, onToggle }: {
    turn: HistoryTurn; open: boolean; onToggle: () => void;
}) {
    const user = turn.user.map(contentText).filter(Boolean).join('\n\n');
    const calls = turn.steps.reduce((count, step) => count + step.tool_calls, 0);
    return (
        <section data-testid="history-turn" className="min-w-0 space-y-3 rounded-lg border
            border-line bg-raised px-3 py-3">
            <button type="button" onClick={onToggle}
                aria-expanded={open}
                className="w-full min-w-0 break-words [overflow-wrap:anywhere] text-left
                    text-xs text-ink-muted hover:text-ink">
                turn {turn.index + 1} · {user.slice(0, 100) || '(empty input)'}
                {user.length > 100 ? '…' : ''}
            </button>
            {open && <div className="min-w-0 space-y-3">
                <div className="ml-auto w-fit max-w-full rounded-lg bg-accent px-3 py-2 text-accent-ink">
                    <p className="whitespace-pre-wrap break-words [overflow-wrap:anywhere] text-sm">
                        {user || '(empty input)'}
                    </p>
                </div>
                {turn.steps.map((step) => {
                    const answer = step.content.map(contentText).filter(Boolean).join('\n\n');
                    return (
                        <div key={step.index} className="min-w-0 space-y-1">
                            {step.reasoning?.raw && (
                                <details className="min-w-0 rounded border border-line bg-sunken px-2 py-1">
                                    <summary className="cursor-pointer text-xs text-ink-muted">reasoning</summary>
                                    <p className="whitespace-pre-wrap break-words [overflow-wrap:anywhere] text-xs">
                                        {step.reasoning.raw}
                                    </p>
                                </details>
                            )}
                            {answer && <div className="min-w-0 break-words [overflow-wrap:anywhere] text-sm">
                                <Markdown>{answer}</Markdown>
                            </div>}
                            {(step.omitted_parts ?? 0) > 0 && <p className="text-xs text-ink-muted">
                                {step.omitted_parts} response parts omitted
                            </p>}
                            {step.tool_calls > 0 && <p className="text-xs text-ink-muted">
                                {step.tool_calls} tool call{step.tool_calls === 1 ? '' : 's'}
                            </p>}
                        </div>
                    );
                })}
                {turn.omitted_steps > 0 && <p className="text-xs text-ink-muted">
                    loading {turn.omitted_steps} more model steps…
                </p>}
                {(turn.omitted_user_parts ?? 0) > 0 && <p className="text-xs text-ink-muted">
                    {turn.omitted_user_parts} input part{turn.omitted_user_parts === 1 ? '' : 's'} omitted
                </p>}
                {calls > 0 && <p className="text-xs text-ink-muted">
                    {calls} tool call{calls === 1 ? '' : 's'} in this turn
                </p>}
            </div>}
        </section>
    );
}

/** One model response: reasoning, markdown, the calls it proposed, and cost. */
function AssistantMessage({ block, calls }: {
    block: AssistantBlock;
    calls: ReadonlyMap<string, ToolCall>;
}) {
    const proposed = block.callIds
        .map((key) => calls.get(key))
        .filter((call): call is ToolCall => call !== undefined);

    return (
        <article data-testid="assistant-message" className="space-y-2">
            <p className="flex items-baseline gap-2 text-xs text-ink-faint">
                <span className="font-medium text-ink-muted">assistant</span>
                {block.clock && <span>{block.clock}</span>}
                {block.cost && <span>{block.cost}</span>}
            </p>

            {block.reasoning && (
                <details className="rounded border border-line bg-sunken px-2 py-1">
                    <summary className="cursor-pointer select-none text-xs text-ink-muted">
                        reasoning
                    </summary>
                    <div className="mt-1 text-sm text-ink-muted">
                        <Markdown>{block.reasoning}</Markdown>
                    </div>
                </details>
            )}

            {block.text
                ? <Markdown>{block.text}</Markdown>
                : <p className="text-sm italic text-ink-faint">(no text in this response)</p>}

            {proposed.length > 0 && (
                <div className="space-y-1">
                    {proposed.map((call) => <ToolCard key={call.key} call={call} />)}
                </div>
            )}
        </article>
    );
}

/** The one-line summary a folded turn shows. */
function RoundSummary({ round, historicalInput, expanded, onToggle }: {
    round: Round;
    historicalInput: HistoryTurn | null;
    expanded: boolean;
    onToggle: () => void;
}) {
    const parts: string[] = [];
    if (round.status) parts.push(round.status);
    else if (round.open) parts.push('running');
    if (round.assistant.length > 0) {
        parts.push(`${round.assistant.length} repl${round.assistant.length === 1 ? 'y' : 'ies'}`);
    }
    if (round.calls.length > 0) {
        parts.push(`${round.calls.length} tool${round.calls.length === 1 ? '' : 's'}`);
    }
    if (round.exchanges !== null) parts.push(`${round.exchanges} exchange(s)`);
    if (round.wallMs !== null) parts.push(formatDuration(round.wallMs));
    if (round.tokens !== null) parts.push(`${round.tokens} tokens`);
    const hidden = round.protocol.length;
    if (hidden > 0) parts.push(`${hidden} protocol event${hidden === 1 ? '' : 's'}`);

    const preview = round.continued ? 'continued from worker state' : round.input
        ? round.input.parts.map((part: ContentPart) => part.raw).join(' ').slice(0, 80)
        : round.admitted && historicalInput
            ? historicalInput.user.map(contentText).filter(Boolean).join(' ').slice(0, 80)
            : round.admitted ? '(input replayed without its text)' : '';

    return (
        <button
            type="button"
            data-testid="round-summary"
            aria-expanded={expanded}
            onClick={onToggle}
            className={`flex w-full items-baseline gap-2 rounded border px-2 py-1 text-left
                hover:bg-subtle ${round.failure
                    ? 'border-danger-line bg-danger-soft'
                    : 'border-line bg-sunken'}`}
        >
            <span className="text-xs text-ink-faint">{expanded ? '▾' : '▸'}</span>
            <span className="text-xs font-medium text-ink-muted">turn {round.index}</span>
            {preview && (
                <span className="truncate text-xs text-ink-muted">“{preview}”</span>
            )}
            <span className="flex-1" />
            <span className={`shrink-0 text-xs ${round.failure ? 'text-danger' : 'text-ink-faint'}`}>
                {parts.join(' · ')}
            </span>
        </button>
    );
}

/** Everything in a round, in the order it happened. */
function RoundBody({ round, historicalInput, showDetails }: {
    round: Round;
    historicalInput: HistoryTurn | null;
    showDetails: boolean;
}) {
    const calls = useMemo(() => {
        const index = new Map<string, ToolCall>();
        for (const call of round.calls) index.set(call.key, call);
        return index;
    }, [round.calls]);

    const assistant = useMemo(() => {
        const index = new Map<string, AssistantBlock>();
        for (const block of round.assistant) index.set(block.key, block);
        return index;
    }, [round.assistant]);

    const problems = useMemo(() => {
        const index = new Map<string, Problem>();
        for (const problem of round.problems) index.set(problem.key, problem);
        return index;
    }, [round.problems]);

    const protocol = useMemo(() => {
        const index = new Map<string, WorkerEnvelope>();
        for (const item of round.protocol) index.set(item.id, item.envelope);
        return index;
    }, [round.protocol]);

    const notes = useMemo(() => {
        const index = new Map<string, NoteItem>();
        for (const note of round.notes) index.set(note.id, note);
        return index;
    }, [round.notes]);

    return (
        <div className="space-y-2">
            {round.input ? (
                <UserMessage item={round.input} />
            ) : round.admitted && historicalInput ? (
                <RestoredUserMessage turn={historicalInput} />
            ) : round.admitted ? (
                <AdmittedPlaceholder />
            ) : null}

            {round.failure && <RunFailureNotice failure={round.failure} />}

            {round.timeline.map((entry) => {
                if (entry.kind === 'assistant') {
                    const block = assistant.get(entry.key);
                    return block
                        ? <AssistantMessage key={entry.key} block={block} calls={calls} />
                        : null;
                }
                if (entry.kind === 'calls') {
                    const call = calls.get(entry.key);
                    return call
                        ? <div key={entry.key} className="space-y-1"><ToolCard call={call} /></div>
                        : null;
                }
                if (entry.kind === 'problem') {
                    const problem = problems.get(entry.key);
                    return problem ? <ProblemLine key={entry.key} problem={problem} /> : null;
                }
                if (entry.kind === 'note') {
                    const note = notes.get(entry.key);
                    return note ? <NoteLine key={entry.key} item={note} /> : null;
                }
                const envelope = protocol.get(entry.key);
                // Protocol events are the machinery; the switch is what decides
                // whether they are part of the reading.
                if (!envelope || !showDetails) return null;
                return <ProtocolLine key={entry.key} envelope={envelope} />;
            })}
        </div>
    );
}

export function Transcript() {
    const client = useClient();
    const selected = usePanel((state) => state.selected);
    const session = useSession(selected);
    const view = useView(selected);
    const items = view?.items ?? EMPTY_ITEMS;
    const history = view?.history ?? [];
    const confirmations = view?.confirmations ?? EMPTY_PROMPTS;
    const requests = view?.requests ?? EMPTY_REQUESTS;
    const dropped = view?.droppedItems ?? 0;
    const showDetails = usePanel((state) => state.showDetails);

    const rounds = useMemo(
        () => buildRounds(items, confirmations, requests),
        [items, confirmations, requests],
    );

    // The worker history is a fallback for turns absent from hub replay. Keep
    // detailed live rounds, including their tool cards, when both sources
    // describe the same committed input. The projection supplies the missing
    // user text for an admitted input replayed without its panel outbox.
    const { olderHistory, historyForRun } = useMemo(() => {
        const mapped = new Map<string, HistoryTurn>();
        const baseline = view?.historySequence;
        const worker = view?.historyWorker;
        if (view?.historyLoading || baseline === null || baseline === undefined || !worker) {
            return { olderHistory: history, historyForRun: mapped };
        }
        const detailed = rounds.filter((round) => round.kind === 'run'
            && round.protocol.some((item) => item.envelope.event === 'input_committed'
                && item.envelope.worker_id === worker
                && typeof item.envelope.sequence === 'number'
                && item.envelope.sequence <= baseline));
        const count = Math.min(detailed.length, history.length);
        const older = history.slice(0, history.length - count);
        if (count > 0) {
            detailed.slice(-count).forEach((round, index) => {
                mapped.set(round.key, history[older.length + index]!);
            });
        }
        return { olderHistory: older, historyForRun: mapped };
    }, [history, rounds, view?.historyLoading, view?.historySequence, view?.historyWorker]);

    // Which turns the reader has opened or closed by hand. Absent means the
    // default: the most recent few are open.
    const [toggled, setToggled] = useState<ReadonlyMap<string, boolean>>(new Map());
    useEffect(() => setToggled(new Map()), [selected]);
    const [historyToggled, setHistoryToggled] = useState<ReadonlyMap<number, boolean>>(new Map());
    useEffect(() => setHistoryToggled(new Map()), [selected]);

    const openByDefault = useMemo(() => {
        const runs = rounds.filter((round) => round.kind === 'run');
        const open = new Map<string, boolean>();
        runs.forEach((round, position) => {
            open.set(round.key, position >= runs.length - OPEN_ROUNDS);
        });
        return open;
    }, [rounds]);

    const isOpen = (round: Round): boolean => {
        const chosen = toggled.get(round.key);
        if (chosen !== undefined) return chosen;
        return openByDefault.get(round.key) ?? true;
    };
    const toggle = (round: Round): void => {
        setToggled((current) => {
            const next = new Map(current);
            next.set(round.key, !isOpen(round));
            return next;
        });
    };

    const scroller = useRef<HTMLDivElement>(null);
    const [following, setFollowing] = useState(true);

    const measure = useCallback(() => {
        const node = scroller.current;
        if (!node) return;
        const distance = node.scrollHeight - node.scrollTop - node.clientHeight;
        setFollowing(distance <= STICK_THRESHOLD_PX);
    }, []);

    // Only a scroll the reader caused turns following off: appending content
    // does not fire a scroll event, which is what makes this the right signal.
    useEffect(() => {
        const node = scroller.current;
        if (!node) return;
        node.addEventListener('scroll', measure, { passive: true });
        return () => node.removeEventListener('scroll', measure);
    }, [measure, selected]);

    useLayoutEffect(() => {
        const node = scroller.current;
        if (!node) return;
        node.scrollTop = node.scrollHeight;
        setFollowing(true);
    }, [selected]);

    useLayoutEffect(() => {
        if (!following) return;
        const node = scroller.current;
        if (!node) return;
        node.scrollTop = node.scrollHeight;
        // `showDetails` is in the list because the switch inserts and removes
        // content above the fold: without it, turning technical details on
        // while reading the end leaves the view adrift and offers a "jump to
        // latest" button to a reader who never left.
    }, [items, history, following, showDetails]);

    const jumpToLatest = useCallback(() => {
        const node = scroller.current;
        if (!node) return;
        node.scrollTop = node.scrollHeight;
        setFollowing(true);
    }, []);

    if (!selected) {
        return (
            <EmptyState
                className="flex-1"
                icon="empty-session"
                title="No session selected"
                detail="Pick one on the left, or create one and start its worker."
            />
        );
    }

    return (
        <div className="relative flex min-h-0 flex-1 flex-col">
            <div
                ref={scroller}
                data-testid="transcript"
                className="min-h-0 flex-1 space-y-3 overflow-y-auto px-4 py-3"
            >
                {dropped > 0 && (
                    <p className="text-xs text-ink-faint">
                        {dropped} earlier item{dropped === 1 ? '' : 's'} dropped to keep the
                        transcript bounded
                    </p>
                )}

                {view?.historyLoading && (
                    <p className="text-xs text-ink-muted">loading conversation history…</p>
                )}
                {olderHistory.map((turn, index) => <HistoryRound key={turn.index} turn={turn}
                    open={historyToggled.get(turn.index) ?? index >= olderHistory.length - OPEN_ROUNDS}
                    onToggle={() => setHistoryToggled((current) => {
                        const next = new Map(current);
                        const currentOpen = current.get(turn.index)
                            ?? index >= olderHistory.length - OPEN_ROUNDS;
                        next.set(turn.index, !currentOpen);
                        return next;
                    })} />)}

                {!view ? (
                    /* No `subscribed` frame yet: the transcript is on its way,
                       and saying "nothing yet" here would be a claim the panel
                       cannot support. */
                    <LoadingLines label="waiting for this session's transcript" lines={4} />
                ) : rounds.length === 0 && olderHistory.length === 0 && !view.historyLoading ? (
                    <EmptyState
                        icon="empty-session"
                        title="Nothing in this transcript yet"
                        detail={session?.connected
                            ? 'Events appear here as the worker reports them. Send a message to start a run.'
                            : 'No worker is attached. Start one, then send a message.'}
                    />
                ) : rounds.map((round) => (
                    <section
                        key={round.key}
                        data-testid="round"
                        data-kind={round.kind}
                        className="animate-enter"
                    >
                        {round.kind === 'run' && (
                            <RoundSummary
                                round={round}
                                historicalInput={historyForRun.get(round.key) ?? null}
                                expanded={isOpen(round)}
                                onToggle={() => toggle(round)}
                            />
                        )}
                        {(round.kind === 'prelude' || isOpen(round)) && (
                            <div className={round.kind === 'run' ? 'mt-2' : ''}>
                                <RoundBody round={round}
                                    historicalInput={historyForRun.get(round.key) ?? null}
                                    showDetails={showDetails} />
                            </div>
                        )}
                        {round.kind === 'run' && !isOpen(round) && (
                            <div className="mt-1 space-y-1">
                                {round.notes.map((note) => <NoteLine key={note.id} item={note} />)}
                                {round.problems.map((problem) => (
                                    <ProblemLine key={problem.key} problem={problem} />
                                ))}
                            </div>
                        )}
                    </section>
                ))}

                {view?.runActive && session?.connected && (
                    <RunActivity label={activityLabel(items)} />
                )}
            </div>

            {!following && (
                <button
                    type="button"
                    onClick={jumpToLatest}
                    className="absolute bottom-12 left-1/2 inline-flex -translate-x-1/2
                        animate-enter items-center gap-1 rounded-full bg-accent px-3 py-1
                        text-xs font-medium text-accent-ink shadow hover:bg-accent-hover
                        focus-visible:outline-2 focus-visible:outline-offset-2
                        focus-visible:outline-interactive"
                >
                    <Glyph name="latest" size="sm" />
                    jump to latest
                </button>
            )}

            <div className="flex flex-wrap items-center justify-end gap-x-3 gap-y-1 border-t
                border-line px-4 py-1.5 text-xs text-ink-muted">
                <TranscriptStats sessionId={selected} />
            </div>
        </div>
    );
}

/** Counters for the current transcript, including what had to be dropped. */
function TranscriptStats({ sessionId }: { sessionId: string }) {
    const view = useView(sessionId);
    const stats = useMemo(() => statsOf(view), [view]);

    return (
        <span data-testid="transcript-stats" className="flex items-center gap-2">
            <span>{stats.items} items</span>
            <span>seq {stats.lastSeq}</span>
            {stats.gaps > 0 && <span className="text-warn">{stats.gaps} gap(s)</span>}
            {stats.duplicates > 0 && <span>{stats.duplicates} duplicate(s)</span>}
            {stats.confirmations > 0 && (
                <span className="text-warn">{stats.confirmations} approval(s)</span>
            )}
            {stats.unknownRequests > 0 && (
                <span className="text-warn" title="sent, and never answered">
                    {stats.unknownRequests} unanswered
                </span>
            )}
        </span>
    );
}
