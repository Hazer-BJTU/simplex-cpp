/** Correlated read-only queries; neither retained event history nor an answer-sized cache. */
import { newRequestId } from '../protocol/messages.ts';
import type { Session } from '../state/registry.ts';
import type { WorkerConnection, ForwardedEnvelope } from './connection.ts';
import { answerQuery, answerPage, type AnswerQuery, type AnswerPage } from '../../shared/answers.ts';

interface Pending { connection: WorkerConnection; query: AnswerQuery; complete(value: unknown, error?: string): void }
const pending = new Map<string, Pending>();

/** Session/direct-parent authorization belongs to the caller; this verifies the live source. */
export function readAnswer(session: Session, input: unknown, signal?: AbortSignal): Promise<AnswerPage> {
    if (!answerQuery(input)) return Promise.reject(new Error('invalid answer cursor'));
    const query: AnswerQuery = { source: { ...input.source }, part: input.part, offset: input.offset };
    const connection = session.connection as WorkerConnection | null;
    if (!connection?.isOpen || session.closing || session.identity.workerId !== query.source.worker_id
        || session.workerCapabilities?.workerId !== query.source.worker_id
        || !session.workerCapabilities.names.includes('answer-pages')) return Promise.reject(new Error('answer source unavailable'));
    if (pending.size >= 64 || [...pending.values()].filter(item => item.connection === connection).length >= 2)
        return Promise.reject(new Error('answer query capacity exhausted; retry after the outstanding query'));
    if (signal?.aborted) return Promise.reject(new Error('answer query aborted'));
    const id = newRequestId();
    return new Promise((resolve, reject) => {
        const abort = (): void => finish(null, 'answer query aborted');
        const close = (): void => finish(null, 'answer worker disconnected');
        const timer = setTimeout(() => finish(null, 'answer query timed out'), 10000);
        timer.unref();
        function finish(value: unknown, error?: string): void {
            if (!pending.delete(id)) return;
            clearTimeout(timer);
            signal?.removeEventListener('abort', abort);
            connection!.ws.off('close', close);
            if (error) { reject(new Error(error)); return; }
            if (session.connection !== connection || session.closing
                || session.identity.workerId !== query.source.worker_id || !answerPage(value, query)) {
                reject(new Error('answer page identity or cursor mismatch')); return;
            }
            resolve(value);
        }
        pending.set(id, { connection, query, complete: finish });
        signal?.addEventListener('abort', abort, { once: true });
        connection.ws.once('close', close);
        try {
            const sent = connection.sendPayload({ type: 'payload', data: {
                operation: 'answer', request_id: id, ...query,
            } });
            if (!sent.ok) finish(null, sent.error ?? 'answer query not sent');
        } catch {
            finish(null, 'answer query not sent');
        }
    });
}

/** Invoked only after the authenticated event socket verifies its session/worker identity. */
export function receiveAnswer(envelope: ForwardedEnvelope, connection: WorkerConnection): void {
    if (envelope.event !== 'answer' && envelope.event !== 'answer_error') return;
    const value = envelope.data as Record<string, unknown> | null;
    const item = typeof value?.request_id === 'string' ? pending.get(value.request_id) : null;
    if (!item || item.connection !== connection || envelope.worker_id !== item.query.source.worker_id) return;
    item.complete(value, envelope.event === 'answer_error' ? 'answer source expired or unavailable' : undefined);
}
