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
import type { ContentPart, ConfirmationPrompt, WorkerEnvelope } from '../../../shared/protocol.ts';
import { usePanel, useView } from '../state/usePanel.ts';
import { statsOf, type NoteItem, type OutboxItem, type TranscriptItem } from '../state/view.ts';
import { useClient } from './ClientContext.tsx';
import { Markdown } from './Markdown.tsx';
import { ToolCard } from './ToolCard.tsx';
import { clockOf, formatDuration, prettyJson, str } from './content.ts';
import { buildRounds, type AssistantBlock, type Problem, type Round, type ToolCall } from './rounds.ts';

/** How close to the bottom still counts as "following the end". */
const STICK_THRESHOLD_PX = 48;

/** How many of the most recent turns stay open. */
const OPEN_ROUNDS = 3;

/** Length at which an operator's own message is folded. */
const LONG_MESSAGE_CHARS = 600;

const EMPTY_ITEMS: readonly TranscriptItem[] = [];
const EMPTY_PROMPTS: ReadonlyMap<string, ConfirmationPrompt> = new Map();

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
        <div data-testid="protocol-line" className="text-[11px] text-slate-400">
            <div className="flex items-baseline gap-2">
                <span className="h-px flex-1 bg-slate-200" />
                <span className="font-mono">{label}</span>
                {clock && <span>{clock}</span>}
                <span className="h-px flex-1 bg-slate-200" />
            </div>
            <details className="mt-0.5 text-center">
                <summary className="cursor-pointer select-none">payload</summary>
                <pre className="mt-1 max-h-64 overflow-auto whitespace-pre-wrap break-all rounded
                    bg-slate-50 p-2 text-left font-mono text-[11px] text-slate-700">
                    {prettyJson(envelope.raw ?? envelope.data ?? {})}
                </pre>
            </details>
        </div>
    );
}

/** A note the panel itself added: a replay gap, a restart, a warning. */
function NoteLine({ item }: { item: NoteItem }) {
    const tone = item.tone === 'error'
        ? 'border-rose-300 bg-rose-50 text-rose-800'
        : item.tone === 'warn'
            ? 'border-amber-300 bg-amber-50 text-amber-900'
            : 'border-slate-200 bg-slate-50 text-slate-600';
    return (
        <div data-testid="transcript-note" className={`rounded border px-2 py-1 text-xs ${tone}`}>
            {item.text}
        </div>
    );
}

/** A problem the worker reported. */
function ProblemLine({ problem }: { problem: Problem }) {
    const tone = problem.tone === 'error'
        ? 'border-rose-400 bg-rose-50 text-rose-800'
        : 'border-amber-400 bg-amber-50 text-amber-900';
    return (
        <div data-testid="transcript-problem" className={`rounded border-l-2 px-2 py-1 ${tone}`}>
            <p className="text-[11px] font-medium">{problem.label}</p>
            <p className="text-xs">{problem.text}</p>
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
            className="ml-auto w-fit max-w-[80%] rounded-lg bg-slate-900 px-3 py-2 text-white"
        >
            <p className="whitespace-pre-wrap break-words text-sm">{shown}</p>
            {long && (
                <button
                    type="button"
                    className="mt-1 text-[11px] text-slate-400 hover:text-slate-200"
                    onClick={() => setOpen((value) => !value)}
                >
                    {open ? 'show less' : `show all ${text.length} characters`}
                </button>
            )}
            <p className="mt-1 text-[11px] text-slate-400">
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
            className="rounded border border-dashed border-slate-300 px-2 py-1 text-[11px]
                italic text-slate-500"
        >
            user input — the worker protocol reports that an input was admitted, not what it said
        </p>
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
            <p className="flex items-baseline gap-2 text-[11px] text-slate-400">
                <span className="font-medium text-slate-500">assistant</span>
                {block.clock && <span>{block.clock}</span>}
                {block.cost && <span>{block.cost}</span>}
            </p>

            {block.reasoning && (
                <details className="rounded border border-slate-200 bg-slate-50 px-2 py-1">
                    <summary className="cursor-pointer select-none text-[11px] text-slate-500">
                        reasoning
                    </summary>
                    <div className="mt-1 text-[13px] text-slate-600">
                        <Markdown>{block.reasoning}</Markdown>
                    </div>
                </details>
            )}

            {block.text
                ? <Markdown>{block.text}</Markdown>
                : <p className="text-sm italic text-slate-400">(no text in this response)</p>}

            {proposed.length > 0 && (
                <div className="space-y-1">
                    {proposed.map((call) => <ToolCard key={call.key} call={call} />)}
                </div>
            )}
        </article>
    );
}

/** The one-line summary a folded turn shows. */
function RoundSummary({ round, expanded, onToggle }: {
    round: Round;
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

    const preview = round.input
        ? round.input.parts.map((part: ContentPart) => part.raw).join(' ').slice(0, 80)
        : round.admitted ? '(input replayed without its text)' : '';

    return (
        <button
            type="button"
            data-testid="round-summary"
            aria-expanded={expanded}
            onClick={onToggle}
            className="flex w-full items-baseline gap-2 rounded border border-slate-200 bg-slate-50
                px-2 py-1 text-left hover:bg-slate-100"
        >
            <span className="text-[11px] text-slate-400">{expanded ? '▾' : '▸'}</span>
            <span className="text-[11px] font-medium text-slate-500">turn {round.index}</span>
            {preview && (
                <span className="truncate text-[11px] text-slate-600">“{preview}”</span>
            )}
            <span className="flex-1" />
            <span className="shrink-0 text-[11px] text-slate-400">{parts.join(' · ')}</span>
        </button>
    );
}

/** Everything in a round, in the order it happened. */
function RoundBody({ round, showDetails }: { round: Round; showDetails: boolean }) {
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
            ) : round.admitted ? (
                <AdmittedPlaceholder />
            ) : null}

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
    const view = useView(selected);
    const items = view?.items ?? EMPTY_ITEMS;
    const confirmations = view?.confirmations ?? EMPTY_PROMPTS;
    const dropped = view?.droppedItems ?? 0;
    const showDetails = usePanel((state) => state.showDetails);

    const rounds = useMemo(
        () => buildRounds(items, confirmations),
        [items, confirmations],
    );

    // Which turns the reader has opened or closed by hand. Absent means the
    // default: the most recent few are open.
    const [toggled, setToggled] = useState<ReadonlyMap<string, boolean>>(new Map());
    useEffect(() => setToggled(new Map()), [selected]);

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
    }, [items, following, showDetails]);

    const jumpToLatest = useCallback(() => {
        const node = scroller.current;
        if (!node) return;
        node.scrollTop = node.scrollHeight;
        setFollowing(true);
    }, []);

    if (!selected) {
        return (
            <div className="grid flex-1 place-items-center p-8 text-center">
                <div>
                    <p className="text-sm font-medium text-slate-700">No session selected</p>
                    <p className="mt-1 text-xs text-slate-500">
                        Pick one on the left, or create one and start its worker.
                    </p>
                </div>
            </div>
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
                    <p className="text-[11px] text-slate-400">
                        {dropped} earlier item{dropped === 1 ? '' : 's'} dropped to keep the
                        transcript bounded
                    </p>
                )}

                {rounds.length === 0 ? (
                    <p className="py-8 text-center text-xs text-slate-500">
                        Nothing yet. Events appear here as the worker reports them.
                    </p>
                ) : rounds.map((round) => (
                    <section key={round.key} data-testid="round" data-kind={round.kind}>
                        {round.kind === 'run' && (
                            <RoundSummary
                                round={round}
                                expanded={isOpen(round)}
                                onToggle={() => toggle(round)}
                            />
                        )}
                        {(round.kind === 'prelude' || isOpen(round)) && (
                            <div className={round.kind === 'run' ? 'mt-2' : ''}>
                                <RoundBody round={round} showDetails={showDetails} />
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
            </div>

            {!following && (
                <button
                    type="button"
                    onClick={jumpToLatest}
                    className="absolute bottom-12 left-1/2 -translate-x-1/2 rounded-full
                        bg-slate-900 px-3 py-1 text-xs font-medium text-white shadow
                        hover:bg-slate-700"
                >
                    jump to latest
                </button>
            )}

            <div className="flex items-center gap-3 border-t border-slate-100 px-4 py-1.5
                text-[11px] text-slate-500">
                <button
                    type="button"
                    onClick={() => client.reloadTranscript(selected)}
                    className="hover:text-slate-800"
                    title="ask the hub to re-send this session's whole transcript"
                >
                    reload transcript
                </button>
                <span className="flex-1" />
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
            {stats.gaps > 0 && <span className="text-amber-700">{stats.gaps} gap(s)</span>}
            {stats.duplicates > 0 && <span>{stats.duplicates} duplicate(s)</span>}
            {stats.confirmations > 0 && (
                <span className="text-amber-700">{stats.confirmations} approval(s)</span>
            )}
            {stats.unknownRequests > 0 && (
                <span className="text-amber-700" title="sent, and never answered">
                    {stats.unknownRequests} unanswered
                </span>
            )}
        </span>
    );
}
