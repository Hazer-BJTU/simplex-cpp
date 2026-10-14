/** Identity of one committed answer. Preserve every field unchanged when paging. */
export interface AnswerSource {
    /** Worker incarnation that owns the committed response. */
    worker_id: string;
    /** Zero-based user turn in the worker's current history. */
    turn: number;
    /** Zero-based exchange within that turn. */
    step: number;
    /** Positive decimal commit counter, encoded as a string to preserve precision. */
    commit_sequence: string;
    /** SHA-256 committed-answer fingerprint; detects stale or changed content. */
    fingerprint: string;
}

/** Read one exact-answer page; offsets are UTF-8 bytes, not JavaScript characters. */
export interface AnswerQuery {
    /** Complete identity obtained from a committed response/history projection. */
    source: AnswerSource;
    /** Zero-based content part to read. */
    part: number;
    /** UTF-8 byte offset inside that part; start at zero. */
    offset: number;
}

/** Exact content page. Validate identity, requested cursor and forward progress before appending. */
export interface AnswerPage extends AnswerQuery {
    /** Correlation ID assigned to the worker query; not an execution or run ID. */
    request_id: string;
    /** This page's content, preserving the original text rather than a preview. */
    raw: string;
    /** Content encoding/category reported by the worker. Unknown strings are possible. */
    type: string;
    /** Media category of the content part; independent of its encoding. */
    modality: string;
    /** Complete UTF-8 byte length of the current part, not just this page. */
    bytes: number;
    /** Byte offset immediately after raw within the current part. */
    next_offset: number;
    /** Current part until exhausted, then the next part index. Reset offset when advancing. */
    next_part: number;
    /** Total number of committed content parts. */
    total_parts: number;
    /** True only when this page completes the last part of the answer. */
    done: boolean;
}
