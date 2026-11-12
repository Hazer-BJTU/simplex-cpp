/**
 * @file pending ask_user questions.
 *
 * A question is answered *inside* the running loop, and the model is waiting
 * for it: the card therefore belongs to the conversation, not to a dialog.
 * It never steals focus, never covers the transcript, and disappears when the
 * hub settles it. Options are shortcuts; the text field is always available,
 * because the model's choices are suggestions rather than a closed set.
 */
import { useEffect, useState } from 'react';
import type { OperatorQuestion } from '../../../ui/index.ts';
import { usePanel } from '../state/usePanel.ts';
import { Button } from '../ui/Button.tsx';
import { useClient } from './ClientContext.tsx';

const EMPTY: readonly OperatorQuestion[] = [];

/** Mirrors the Hub's answer limit; the wire limit is UTF-8 bytes. */
const MAX_ANSWER_BYTES = 8 * 1024;

function countdown(deadline: number, now: number): string {
    const seconds = Math.max(0, Math.ceil((deadline - now) / 1000));
    return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`;
}

/** One question, one answer. Answering or timing out removes the card. */
function QuestionCard({ session, question }: { session: string; question: OperatorQuestion }) {
    const client = useClient();
    const failure = usePanel(state => state.questionFailures.get(question.question_id));
    const clearFailure = usePanel(state => state.clearQuestionFailure);
    const [text, setText] = useState('');
    const [sending, setSending] = useState(false);
    const [now, setNow] = useState(() => Date.now());
    useEffect(() => {
        const timer = setInterval(() => setNow(Date.now()), 1000);
        return () => clearInterval(timer);
    }, []);
    // A refusal is per question; the card becomes editable again instead of
    // staying in "sending" until the deadline removes it.
    useEffect(() => {
        if (failure !== undefined) setSending(false);
    }, [failure]);

    const value = text.trim();
    const tooLong = new TextEncoder().encode(value).length > MAX_ANSWER_BYTES;
    const canSend = value.length > 0 && !tooLong && !sending;

    function submit(answer: { text?: string; option_index?: number }): void {
        if (sending) return;
        clearFailure(question.question_id);
        if (client.answerQuestion(session, question.question_id, answer)) setSending(true);
    }

    function submitText(): void {
        if (!canSend) return;
        submit({ text: value });
    }

    return (
        <article
            aria-label="Operator question"
            className="rounded-lg border border-line bg-raised p-3 shadow-sm"
        >
            <div className="mb-2 flex items-center justify-between gap-2 text-xs text-ink-muted">
                <span className="flex items-center gap-1.5 font-medium text-ink">
                    <span aria-hidden className="size-1.5 rounded-full bg-accent" />
                    Question from the agent
                </span>
                <span className="tabular-nums">
                    {sending ? 'sending…' : `${countdown(question.deadline_at, now)} left`}
                </span>
            </div>
            <p className="max-h-40 overflow-y-auto whitespace-pre-wrap text-sm text-ink">
                {question.question}
            </p>
            {question.options.length > 0 && (
                <div className="mt-2 flex flex-wrap gap-1.5">
                    {question.options.map((option, index) => (
                        <Button key={option} variant="secondary"
                            disabled={sending}
                            onClick={() => submit({ option_index: index })}>
                            {option}
                        </Button>
                    ))}
                </div>
            )}
            <div className="mt-2 flex items-end gap-2">
                <textarea
                    value={text}
                    onChange={(event) => setText(event.target.value)}
                    onKeyDown={(event) => {
                        // An IME candidate is confirmed with Enter; that keystroke
                        // belongs to the composition, not to the answer.
                        if (event.nativeEvent.isComposing) return;
                        if (event.key === 'Enter' && !event.shiftKey) {
                            event.preventDefault();
                            if (canSend) submitText();
                        }
                    }}
                    rows={2}
                    disabled={sending}
                    placeholder={question.options.length > 0
                        ? 'Or type a different answer…' : 'Type your answer…'}
                    aria-label="Answer"
                    className="min-h-9 flex-1 resize-y rounded border border-line bg-surface p-2 text-sm
                        focus:outline-none focus-visible:outline-2 focus-visible:outline-interactive"
                />
                <Button variant="primary" disabled={!canSend} onClick={submitText}>
                    Answer
                </Button>
            </div>
            {tooLong && (
                <p role="status" className="mt-1 text-xs text-danger">
                    The answer is longer than {MAX_ANSWER_BYTES / 1024} KiB.
                    Shorten it before sending.
                </p>
            )}
            {failure !== undefined && (
                <p role="status" className="mt-1 text-xs text-danger">
                    {failure.message} Edit the answer and try again.
                </p>
            )}
            <p className="mt-1 text-xs text-ink-faint">
                The run is waiting for you. If you do not answer, it continues on its own and the
                agent is told there was no reply.
            </p>
        </article>
    );
}

/** Inline strip above the composer; renders nothing when no question is pending. */
export function OperatorQuestions() {
    const selected = usePanel(state => state.selected);
    const questions = usePanel(state => (state.selected
        ? state.questions.get(state.selected) ?? EMPTY : EMPTY));
    if (!selected || questions.length === 0) return null;
    return (
        <section aria-label="Pending operator questions"
            className="flex flex-col gap-2 border-t border-line bg-subtle px-3 py-2">
            {questions.map(question => (
                <QuestionCard key={question.question_id} session={selected} question={question} />
            ))}
        </section>
    );
}
