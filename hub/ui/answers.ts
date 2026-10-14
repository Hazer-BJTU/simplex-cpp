/** Read-only answer pagination shared by Hub and panel; offsets count UTF-8 bytes. */
export interface AnswerSource {
    worker_id: string;
    turn: number;
    step: number;
    commit_sequence: string;
    /** Required committed-answer fingerprint; copy it unchanged from the worker. */
    fingerprint: string;
}
export interface AnswerQuery { source: AnswerSource; part: number; offset: number }
export interface AnswerPage extends AnswerQuery {
    request_id: string;
    raw: string;
    type: string;
    modality: string;
    bytes: number;
    next_offset: number;
    next_part: number;
    total_parts: number;
    done: boolean;
}
