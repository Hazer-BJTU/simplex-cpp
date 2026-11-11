/**
 * Operator questions raised by the worker's `ask_user` tool.
 *
 * A pending question exists only in Hub memory: it belongs to one live tool
 * request, is answered by any connected panel, and disappears with the process.
 * Nothing here is persisted, replayed or restored after a restart.
 */

export const MAX_QUESTION_BYTES = 8 * 1024;
export const MAX_ANSWER_BYTES = 8 * 1024;
export const MAX_OPTIONS = 8;
export const MIN_OPTIONS = 2;
export const MAX_OPTION_BYTES = 200;

/** One question as the panel displays it. */
export interface PendingQuestion {
    /** Per-request correlation id; unique for the Hub's lifetime. */
    question_id: string;
    /** Question text written by the model. */
    question: string;
    /** Optional choices; free text remains available regardless. */
    options: string[];
    /** Epoch milliseconds when the question reached the Hub. */
    asked_at: number;
    /** Epoch milliseconds when an unanswered question settles as a timeout. */
    deadline_at: number;
}

/** One operator answer: exactly one of free text or an option index. */
export interface QuestionAnswer {
    text?: string;
    option_index?: number;
}

/** What the waiting tool receives. */
export type QuestionResult =
    | { outcome: 'answered'; reply: { text: string } | { option_index: number; option: string } }
    | { outcome: 'timeout' }
    | { outcome: 'cancelled'; reason: string };

export type AnswerResult =
    | { ok: true }
    | { ok: false; code: 'unknown_question' | 'invalid_answer' | 'question_answered'; message: string };

/** A capacity refusal the caller turns into a tool rejection. */
export class QuestionLimitError extends Error {}

const utf8 = (value: string): boolean =>
    Buffer.from(value, 'utf8').toString('utf8') === value;

/** Question text: nonblank, bounded, valid UTF-8. */
export function validQuestion(value: unknown): value is string {
    return typeof value === 'string' && value.trim().length > 0
        && Buffer.byteLength(value) <= MAX_QUESTION_BYTES && utf8(value);
}

/** Choices: 2..8 distinct nonblank strings, each bounded. */
export function validOptions(value: unknown): value is string[] {
    if (!Array.isArray(value) || value.length < MIN_OPTIONS || value.length > MAX_OPTIONS) return false;
    if (!value.every(option => typeof option === 'string' && option.trim().length > 0
        && Buffer.byteLength(option) <= MAX_OPTION_BYTES && utf8(option))) return false;
    return new Set(value).size === value.length;
}

/** Free-text answer: nonblank, bounded, valid UTF-8. */
export function validAnswerText(value: unknown): value is string {
    return typeof value === 'string' && value.trim().length > 0
        && Buffer.byteLength(value) <= MAX_ANSWER_BYTES && utf8(value);
}

interface Entry {
    session: string;
    doc: PendingQuestion;
    resolve(result: QuestionResult): void;
    timer: NodeJS.Timeout;
}

/**
 * Pending questions keyed by request id. Bounded per session and in total so a
 * single run cannot occupy every tool connection or flood the panel.
 */
export class QuestionStore {
    private readonly entries = new Map<string, Entry>();
    private readonly perSession: number;
    private readonly total: number;

    constructor(perSession = 4, total = 64) {
        this.perSession = perSession;
        this.total = total;
    }

    /** Register one question and resolve when answered, timed out or cancelled. */
    open(input: {
        session: string;
        questionId: string;
        question: string;
        options: string[];
        timeoutMs: number;
    }): Promise<QuestionResult> {
        if (this.entries.size >= this.total) {
            throw new QuestionLimitError('too many pending questions on this hub');
        }
        let sessionCount = 0;
        for (const entry of this.entries.values()) {
            if (entry.session === input.session) sessionCount += 1;
        }
        if (sessionCount >= this.perSession) {
            throw new QuestionLimitError('too many pending questions for this session');
        }
        if (this.entries.has(input.questionId)) {
            throw new QuestionLimitError('question request id is already pending');
        }
        const asked = Date.now();
        const doc: PendingQuestion = {
            question_id: input.questionId,
            question: input.question,
            options: [...input.options],
            asked_at: asked,
            deadline_at: asked + input.timeoutMs,
        };
        return new Promise<QuestionResult>((resolve) => {
            const settle = (result: QuestionResult): void => {
                const current = this.entries.get(doc.question_id);
                if (!current || current.resolve !== settle) return;
                clearTimeout(current.timer);
                this.entries.delete(doc.question_id);
                resolve(result);
            };
            const timer = setTimeout(() => settle({ outcome: 'timeout' }),
                Math.max(0, doc.deadline_at - Date.now()));
            timer.unref();
            this.entries.set(doc.question_id, {
                session: input.session,
                doc,
                resolve: settle,
                timer,
            });
        });
    }

    /** Answer one pending question. The first accepted answer wins. */
    answer(session: string, questionId: string, answer: QuestionAnswer): AnswerResult {
        const entry = this.entries.get(questionId);
        if (!entry || entry.session !== session) {
            return { ok: false, code: 'unknown_question',
                message: 'that question is no longer open' };
        }
        const hasText = answer.text !== undefined;
        const hasOption = answer.option_index !== undefined;
        if (hasText === hasOption) {
            return { ok: false, code: 'invalid_answer',
                message: 'provide exactly one of text or option_index' };
        }
        if (hasText) {
            if (!validAnswerText(answer.text)) {
                return { ok: false, code: 'invalid_answer',
                    message: `answer must be 1..${MAX_ANSWER_BYTES} bytes of UTF-8 text` };
            }
            entry.resolve({ outcome: 'answered', reply: { text: answer.text } });
            return { ok: true };
        }
        const index = answer.option_index as number;
        const option = Number.isSafeInteger(index) && index >= 0
            ? entry.doc.options[index] : undefined;
        if (option === undefined) {
            return { ok: false, code: 'invalid_answer',
                message: 'option_index does not name one of this question\'s options' };
        }
        entry.resolve({ outcome: 'answered', reply: { option_index: index, option } });
        return { ok: true };
    }

    /** Withdraw one question, e.g. when its transport closed. */
    withdraw(questionId: string, reason: string): void {
        this.entries.get(questionId)?.resolve({ outcome: 'cancelled', reason });
    }

    /** Withdraw every question of one session, e.g. when its run is cancelled. */
    cancel(session: string, reason: string): void {
        for (const entry of [...this.entries.values()]) {
            if (entry.session === session) entry.resolve({ outcome: 'cancelled', reason });
        }
    }

    /** Withdraw everything, e.g. on Hub shutdown. */
    cancelAll(reason: string): void {
        for (const entry of [...this.entries.values()]) {
            entry.resolve({ outcome: 'cancelled', reason });
        }
    }

    /** Pending questions for one session, oldest first. */
    list(session: string): PendingQuestion[] {
        return [...this.entries.values()]
            .filter(entry => entry.session === session)
            .map(entry => entry.doc)
            .sort((left, right) => left.asked_at - right.asked_at);
    }

    count(session: string): number {
        let total = 0;
        for (const entry of this.entries.values()) {
            if (entry.session === session) total += 1;
        }
        return total;
    }
}
