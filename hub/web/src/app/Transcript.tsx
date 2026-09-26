/**
 * @file the transcript.
 *
 * Minimal on purpose: this stage proves that real events arrive, in order, and
 * that the operator's own messages survive. Markdown, syntax highlighting, tool
 * cards and run grouping are the next stage — what matters here is that the
 * structure they will attach to is right.
 *
 * Two behaviours are already the corrected ones rather than the old panel's:
 *
 * - **Scrolling follows only when the reader is already at the bottom** (defect
 *   D24). The old panel jumped to the end on every `subscribed` and `snapshot`,
 *   so reading history and pressing Status yanked the view away.
 * - **Protocol events are quiet and conversation is not**. The old panel gave
 *   `persisted` the same card as a model response, which is most of why the
 *   output read as noise.
 */
import { useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react';
import type { ContentPart, WorkerEnvelope } from '../../../shared/protocol.ts';
import { usePanel, useView } from '../state/usePanel.ts';
import { statsOf, type OutboxItem, type TranscriptItem } from '../state/view.ts';
import { useClient } from './ClientContext.tsx';
import { clockOf, describeEnvelope, type CallView, type ResultView } from './envelope.ts';

/** How close to the bottom still counts as "following the end". */
const STICK_THRESHOLD_PX = 48;

/** A stable empty list, so the selector does not build a new one every render. */
const EMPTY: readonly TranscriptItem[] = [];

/** The parts of one envelope that are worth showing, compactly. */
function EnvelopeBody({ envelope }: { envelope: WorkerEnvelope }) {
    const view = describeEnvelope(envelope);

    switch (view.kind) {
        case 'message':
            return (
                <div className="space-y-1.5">
                    {view.reasoning && (
                        <details className="text-xs text-slate-500">
                            <summary className="cursor-pointer select-none">reasoning</summary>
                            <p className="mt-1 whitespace-pre-wrap border-l-2 border-slate-200 pl-2">
                                {view.reasoning}
                            </p>
                        </details>
                    )}
                    {view.text
                        ? <p className="whitespace-pre-wrap break-words">{view.text}</p>
                        : <p className="text-sm italic text-slate-400">(no text in this response)</p>}
                    {view.calls.length > 0 && (
                        <ul className="space-y-1">
                            {view.calls.map((call) => <CallRow key={call.id || call.name} call={call} />)}
                        </ul>
                    )}
                    {view.cost && <p className="text-[11px] text-slate-400">{view.cost}</p>}
                </div>
            );
        case 'calls':
            return (
                <ul className="space-y-1">
                    {view.calls.map((call) => <CallRow key={call.id || call.name} call={call} />)}
                </ul>
            );
        case 'results':
            return (
                <ul className="space-y-1">
                    {view.results.map((result, index) => (
                        <ResultRow key={result.id || `${result.name}-${index}`} result={result} />
                    ))}
                </ul>
            );
        case 'admitted':
            // The text of an admitted input is never sent back by the worker,
            // so a replayed one cannot be shown. Saying so beats an empty card.
            return (
                <p className="text-xs italic text-slate-400">
                    user input — the worker protocol does not report its text
                </p>
            );
        case 'problem':
            return (
                <div className="rounded border-l-2 border-rose-400 bg-rose-50 px-2 py-1">
                    <p className="text-xs font-medium text-rose-800">{view.label}</p>
                    {view.text && <p className="text-xs text-rose-700">{view.text}</p>}
                </div>
            );
        case 'unknown':
            return (
                <p className="text-xs text-slate-400">
                    {view.label} — no renderer for this event yet
                </p>
            );
        default:
            return null;
    }
}

/** One proposed call. P5 renders `run_command` as a shell command instead. */
function CallRow({ call }: { call: CallView }) {
    return (
        <li className="rounded border border-slate-200 bg-slate-50 px-2 py-1">
            <span className="flex flex-wrap items-baseline gap-2">
                <span className="font-mono text-xs font-medium text-slate-800">{call.name}</span>
                {call.security && (
                    <span className="rounded bg-slate-200 px-1 text-[10px] text-slate-600">
                        {call.security}
                    </span>
                )}
                {call.scheduling && (
                    <span className="text-[10px] text-slate-500">{call.scheduling}</span>
                )}
                {call.id && (
                    <button
                        type="button"
                        title={`copy call id ${call.id}`}
                        onClick={() => { void navigator.clipboard?.writeText(call.id); }}
                        className="font-mono text-[10px] text-slate-500 hover:text-slate-800"
                    >
                        {call.id.slice(0, 8)}
                    </button>
                )}
            </span>
            {call.args && (
                <pre className="mt-1 overflow-x-auto whitespace-pre-wrap break-all font-mono
                    text-[11px] text-slate-700">
                    {call.args}
                </pre>
            )}
        </li>
    );
}

/** One returned result. */
function ResultRow({ result }: { result: ResultView }) {
    const failed = result.error !== null && !result.skipped;
    return (
        <li className={`rounded border-l-2 bg-slate-50 px-2 py-1
            ${failed ? 'border-rose-400' : result.skipped ? 'border-slate-300' : 'border-emerald-400'}`}>
            <span className="flex flex-wrap items-baseline gap-2">
                <span className="font-mono text-xs font-medium text-slate-800">{result.name}</span>
                <span className="text-[10px] text-slate-500">
                    {result.skipped ? 'not run' : failed ? `failed (${result.error?.stage})` : 'ok'}
                </span>
            </span>
            {failed && result.error?.message && (
                <p className="text-xs text-rose-700">{result.error.message}</p>
            )}
            {result.text && (
                <pre className="mt-1 max-h-60 overflow-auto whitespace-pre-wrap break-words
                    font-mono text-[11px] text-slate-700">
                    {result.text}
                </pre>
            )}
        </li>
    );
}

/** A message the operator sent, kept on screen until the worker confirms it. */
function OutboxRow({ item }: { item: OutboxItem }) {
    return (
        <article
            data-testid="outbox-item"
            data-state={item.state}
            className="ml-auto max-w-[80%] rounded-lg bg-slate-900 px-3 py-2 text-white"
        >
            {item.parts.map((part: ContentPart, index) => (
                <p key={index} className="whitespace-pre-wrap break-words text-sm">
                    {part.raw}
                </p>
            ))}
            <p className="mt-1 text-[11px] text-slate-400">
                {item.state === 'admitted' ? 'admitted by the worker' : 'sent, not yet admitted'}
            </p>
        </article>
    );
}

/** One line in the transcript. */
function Item({ item }: { item: TranscriptItem }) {
    if (item.kind === 'note') {
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
    if (item.kind === 'outbox') return <OutboxRow item={item} />;
    if (item.kind === 'request') {
        const { request } = item;
        return (
            <div data-testid="request-chip" className="text-[11px] text-slate-500">
                request {request.operation} · {request.state}
                {request.detail ? ` · ${request.detail}` : ''}
            </div>
        );
    }

    const view = describeEnvelope(item.envelope);
    const clock = clockOf(item.envelope);
    if (view.kind === 'protocol') {
        return (
            <div data-testid="protocol-line" className="flex items-center gap-2 text-[11px] text-slate-400">
                <span className="h-px flex-1 bg-slate-200" />
                <span>{view.label}</span>
                {clock && <span>{clock}</span>}
                <span className="h-px flex-1 bg-slate-200" />
            </div>
        );
    }
    const isAssistant = view.kind === 'message' || view.kind === 'calls' || view.kind === 'results';
    return (
        <article
            data-testid="transcript-event"
            data-event={item.envelope.event}
            className={isAssistant
                ? 'max-w-[85%] rounded-lg border border-slate-200 bg-white px-3 py-2 text-slate-900'
                : 'text-slate-700'}
        >
            {isAssistant && (
                <p className="mb-1 flex items-baseline gap-2 text-[11px] text-slate-400">
                    <span className="font-medium text-slate-500">
                        {item.envelope.event === 'model_response' ? 'assistant' : 'tools'}
                    </span>
                    {clock && <span>{clock}</span>}
                </p>
            )}
            <EnvelopeBody envelope={item.envelope} />
        </article>
    );
}

export function Transcript() {
    const client = useClient();
    const selected = usePanel((state) => state.selected);
    const items = usePanel((state) => (state.selected
        ? state.views.get(state.selected)?.items ?? EMPTY
        : EMPTY));
    const dropped = usePanel((state) => (state.selected
        ? state.views.get(state.selected)?.droppedItems ?? 0
        : 0));

    const scroller = useRef<HTMLDivElement>(null);
    const [following, setFollowing] = useState(true);

    const measure = useCallback(() => {
        const node = scroller.current;
        if (!node) return;
        const distance = node.scrollHeight - node.scrollTop - node.clientHeight;
        setFollowing(distance <= STICK_THRESHOLD_PX);
    }, []);

    // Only a scroll the *reader* caused turns following off. Appending content
    // does not fire a scroll event, which is what makes this the right signal —
    // measuring after the append would see the new height and always conclude
    // the reader had scrolled away.
    useEffect(() => {
        const node = scroller.current;
        if (!node) return;
        node.addEventListener('scroll', measure, { passive: true });
        return () => node.removeEventListener('scroll', measure);
    }, [measure, selected]);

    // A different session always opens at its end.
    useLayoutEffect(() => {
        const node = scroller.current;
        if (!node) return;
        node.scrollTop = node.scrollHeight;
        setFollowing(true);
    }, [selected]);

    // Otherwise follow only when the reader had not scrolled away (defect D24:
    // the old panel jumped to the end on every replay).
    useLayoutEffect(() => {
        if (!following) return;
        const node = scroller.current;
        if (!node) return;
        node.scrollTop = node.scrollHeight;
    }, [items, following]);

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
                className="min-h-0 flex-1 space-y-2 overflow-y-auto px-4 py-3"
            >
                {dropped > 0 && (
                    <p className="text-[11px] text-slate-400">
                        {dropped} earlier item{dropped === 1 ? '' : 's'} dropped to keep the
                        transcript bounded
                    </p>
                )}
                {items.length === 0 ? (
                    <p className="py-8 text-center text-xs text-slate-500">
                        Nothing yet. Events appear here as the worker reports them.
                    </p>
                ) : (
                    items.map((item) => <Item key={item.id} item={item} />)
                )}
            </div>

            {!following && (
                <button
                    type="button"
                    onClick={jumpToLatest}
                    className="absolute bottom-3 left-1/2 -translate-x-1/2 rounded-full
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
    const stats = statsOf(useView(sessionId));
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
