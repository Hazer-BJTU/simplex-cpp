/** Literal marker parsing; skill text is inserted once and never recursively parsed. */
import type { SkillDocument } from '../../../ui/index.ts';

export interface SkillMarker { start: number; end: number; id: string; escaped: boolean; }
/** Browser-only recovery data. Never included in the worker payload. */
export interface SkillRecovery {
    draft: string;
    skills: Record<string, SkillDocument>;
}
const MAX_EXPANDED_BYTES = 256 * 1024;

/**
 * Discover complete references independently of whole-draft validation.
 * Invalid or unfinished neighboring markers must not detach loaded snapshots.
 * Escaped references are literal text and do not retain a snapshot.
 */
export function referencedSkillIds(draft: string): string[] {
    return [...new Set(
        [...draft.matchAll(/\\?@\{skill:([A-Za-z0-9_-]{1,128})\}/g)]
            .filter(match => !match[0].startsWith('\\'))
            .map(match => match[1]!),
    )];
}

/** Reject unfinished/invalid references rather than silently sending their spelling. */
export function skillMarkers(draft: string): SkillMarker[] {
    const markers: SkillMarker[] = [];
    const pattern = /\\?@\{skill:/g;
    let match;
    while ((match = pattern.exec(draft))) {
        const escaped = match[0].startsWith('\\');
        const end = draft.indexOf('}', pattern.lastIndex);
        if (end < 0) {
            if (escaped) break;
            throw new Error('Complete the skill reference with } or escape it with a backslash.');
        }
        const id = draft.slice(pattern.lastIndex, end);
        if (!escaped && !/^[A-Za-z0-9_-]{1,128}$/.test(id)) throw new Error('Invalid skill ID in message.');
        markers.push({ start: match.index, end: end + 1, id, escaped });
        pattern.lastIndex = end + 1;
        if (markers.filter(m => !m.escaped).length > 16) throw new Error('Use at most 16 skill references per message.');
    }
    return markers;
}

/** Exact surrounding user text is preserved; escaped references become literal text. */
export function expandSkills(draft: string, skills: Record<string, SkillDocument>): string {
    let output = '';
    let offset = 0;
    for (const marker of skillMarkers(draft)) {
        output += draft.slice(offset, marker.start);
        if (marker.escaped) output += draft.slice(marker.start + 1, marker.end);
        else {
            const skill = Object.hasOwn(skills, marker.id) ? skills[marker.id] : undefined;
            if (!skill) throw new Error(`Skill ${marker.id} is not loaded.`);
            output += skill.text;
        }
        offset = marker.end;
        if (new TextEncoder().encode(output).length > MAX_EXPANDED_BYTES) {
            throw new Error('Expanded message exceeds 256 KiB.');
        }
    }
    output += draft.slice(offset);
    if (new TextEncoder().encode(output).length > MAX_EXPANDED_BYTES) throw new Error('Expanded message exceeds 256 KiB.');
    return output;
}

/** Completion range ending at the caret, excluding escaped and finished markers. */
export function skillCompletion(draft: string, caret: number): { start: number; query: string } | null {
    const before = draft.slice(0, caret);
    const match = /@\{skill:([A-Za-z0-9_-]*)$/.exec(before);
    if (!match || before[match.index - 1] === '\\') return null;
    return { start: match.index, query: match[1]! };
}
