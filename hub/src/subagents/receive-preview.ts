/** Model-facing receive output deliberately excludes transport cursors and sources. */
import type { Conversation } from './conversation.ts';

/** Keep a valid UTF-8 prefix within the model-facing preview allowance. */
function clip(text: string, allowance: number): string {
    const bytes = Buffer.from(text);
    let end = Math.min(bytes.length, allowance);
    while (end > 0 && end < bytes.length && (bytes[end]! & 0xc0) === 0x80) end--;
    return bytes.subarray(0, end).toString('utf8');
}

/** Latest visible response, falling back over empty/tool-only steps within that turn. */
export function receivePreview(conversation: Conversation | null,
    compact?: { request_id: string; summary: string }): Record<string, unknown> {
    const turn = conversation?.turns.at(-1);
    const step = turn?.steps.findLast(item => item.content.some(part => part.raw.length > 0));
    const parts = compact ? [{ type: 'text', modality: 'text', raw: compact.summary }] : step?.content ?? [];
    const requestId = compact?.request_id ?? step?.execution?.request_id;
    let remaining = 16 * 1024;
    let truncated = false;
    const text: string[] = [];
    for (const part of parts) {
        const value = clip(part.raw, Math.max(0, remaining));
        remaining -= Buffer.byteLength(value);
        truncated ||= value !== part.raw || part.truncated === true;
        text.push(value);
    }
    return {
        response: text.join('\n'),
        truncated,
        stale: conversation?.stale ?? true,
        incomplete: conversation?.incomplete ?? true,
        history_truncated: conversation?.truncated ?? false,
        ...(requestId ? { request_id: requestId } : {}),
    };
}
