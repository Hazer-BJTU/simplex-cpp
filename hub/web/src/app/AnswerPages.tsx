import { memo, useEffect, useRef, useState } from 'react';
import { answerSource, answerPage, type AnswerSource, type AnswerPage } from '../../../shared/answers.ts';
import { useClient } from './ClientContext.tsx';
import { usePanel } from '../state/usePanel.ts';
import { CopyButton } from './Markdown.tsx';

/** One page in memory/DOM. Explicit navigation keeps any-sized answers accessible without eager parsing. */
export const AnswerPages = memo(function AnswerPages({ source }: { source: AnswerSource }) {
    const client = useClient();
    const session = usePanel(state => state.selected);
    const [page, setPage] = useState<AnswerPage | null>(null);
    const [busy, setBusy] = useState(false);
    const [error, setError] = useState('');
    const abort = useRef<AbortController | null>(null);
    const sourceKey = JSON.stringify(source);
    useEffect(() => { setPage(null); setError(''); setBusy(false); return () => abort.current?.abort(); }, [sourceKey, session]);
    async function load(part: number, offset: number) {
        if (!session || busy) return;
        const control = new AbortController();
        abort.current?.abort();
        abort.current = control;
        setBusy(true); setError('');
        const query = { source, part, offset };
        try {
            const value = await client.rest.request<unknown>('POST', `/api/sessions/${encodeURIComponent(session)}/answer`,
                { body: query, signal: control.signal });
            if (control.signal.aborted) return;
            if (!answerPage(value, query)) throw new Error('Answer page identity or offset does not match.');
            if (page && offset !== 0 && (value.total_parts !== page.total_parts
                || value.part === page.part && (value.bytes !== page.bytes
                    || value.type !== page.type || value.modality !== page.modality))) {
                throw new Error('Answer source changed between pages. Reload history.');
            }
            setPage(value);
        } catch (failure) {
            if (!control.signal.aborted) setError(failure instanceof Error ? failure.message : 'Answer unavailable.');
        } finally { if (!control.signal.aborted) setBusy(false); }
    }
    return <div className="min-w-0 space-y-2 text-xs text-ink-muted" data-testid="answer-pages">
        <button disabled={busy || !session} onClick={() => void load(0, 0)} className="underline disabled:opacity-50">
            {busy ? 'Loading answer…' : page ? 'Read answer from the beginning' : 'Read complete answer in pages'}
        </button>
        {error && <p role="status">{error}</p>}
        {page && <div className="min-w-0 space-y-2">
            <p>part {page.part + 1}/{page.total_parts} · bytes {page.offset}–{page.next_offset}/{page.bytes}</p>
            <pre className="whitespace-pre-wrap break-words [overflow-wrap:anywhere] text-sm text-ink">{page.raw}</pre>
            <CopyButton text={() => page.raw} label="copy this page" />
            {!page.done && <button disabled={busy} onClick={() => void load(page.next_part,
                page.next_part === page.part ? page.next_offset : 0)} className="ml-3 underline disabled:opacity-50">Next answer page</button>}
            {page.done && <span className="ml-3">End of answer</span>}
        </div>}
    </div>;
});

/** Unknown/older workers never get a nonfunctional "show all" promise. */
export function AnswerAccess({ source, shortened }: { source: unknown; shortened: boolean }) {
    if (!shortened) return null;
    if (!answerSource(source)) return <p className="text-xs text-ink-muted">Answer preview; complete text is unavailable from this worker.</p>;
    return <AnswerPages source={source} />;
}
