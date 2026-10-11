import { profileCount } from '../lib/profile.ts';

const ROUND_SELECTOR = '[data-testid="round"], [data-testid="history-turn"]';
const CHILD_SELECTOR = '[data-testid="assistant-message"], [data-testid="tool-card"], [data-testid="outbox-item"]';

/** First disjoint, vertically ordered item whose end is below the viewport edge. */
export function firstEndingAfter<T>(items: readonly T[], edge: number, bottom: (item: T) => number): number {
    let low = 0;
    let high = items.length;
    while (low < high) {
        const middle = Math.floor((low + high) / 2);
        if (bottom(items[middle]!) > edge) high = middle;
        else low = middle + 1;
    }
    return low;
}

/** Last item starting at/before the edge; nested descendants follow their parents. */
export function lastStartingBefore<T>(items: readonly T[], edge: number, top: (item: T) => number): number {
    let low = 0;
    let high = items.length;
    while (low < high) {
        const middle = Math.floor((low + high) / 2);
        if (top(items[middle]!) <= edge) low = middle + 1;
        else high = middle;
    }
    return low - 1;
}

/** Dimensions that can clamp or reflow the scroll position without user input. */
export interface ScrollGeometry {
    height: number;
    viewport: number;
    width: number;
}

/**
 * Remember the actual clamped/fractional position accepted by the browser.
 * Unlike a task-local flag, this recognizes delayed/coalesced write echoes.
 * Input extends the intent window but does not erase an outstanding echo.
 * A smaller scroll range can clamp scrollTop before our next frame. Only the
 * actual clamp of the previous position is layout-owned; a dimension change
 * alone cannot explain user movement. Fresh input samples the current position
 * and dimensions so subsequent wheel/key/drag movement still takes priority.
 */
export class ScrollOwnership {
    private target: number | null = null;
    private position: number | null = null;
    private intentUntil = 0;
    private geometry: ScrollGeometry | null = null;

    private observe(top: number, geometry?: ScrollGeometry): void {
        this.position = top;
        if (geometry) this.geometry = { ...geometry };
    }

    intent(now: number, top = 0, geometry?: ScrollGeometry): void {
        if (geometry) this.observe(top, geometry);
        this.intentUntil = now + 1000;
    }

    wrote(top: number, geometry?: ScrollGeometry): void {
        this.observe(top, geometry);
        this.target = top;
    }

    /** Explicit navigation supersedes a preceding gesture or keyboard animation. */
    resumeFollowing(): void {
        this.intentUntil = 0;
    }

    readerMoved(top: number, now: number, geometry?: ScrollGeometry): boolean {
        const before = this.geometry;
        const previous = this.position;
        const rangeShrank = geometry && before
            && geometry.height - geometry.viewport < before.height - before.viewport;
        const clamped = geometry && previous !== null
            ? Math.max(0, Math.min(previous, geometry.height - geometry.viewport)) : null;
        // Reflow alone is not proof of automatic movement. Suppress only the
        // exact clamp of an out-of-range previous position, including repeated
        // echoes, and let any additional user movement claim the scroll.
        const layoutClamp = rangeShrank && previous !== null && clamped !== null
            && Math.abs(previous - clamped) > 0.5 && Math.abs(top - clamped) <= 0.5;
        this.observe(top, geometry);
        if (layoutClamp) {
            this.target = top;
            return false;
        }
        if (this.target !== null && Math.abs(top - this.target) <= 0.5) return false;
        this.target = null;
        return now < this.intentUntil;
    }
}

interface ChildIndex {
    elements: Element[];
    parents: Map<Element, Element>;
}

/**
 * Cache membership, not coordinates. Geometry probes stay fresh for disclosure,
 * font, content or viewport changes, and each lookup reads logarithmically many
 * siblings. Only the selected round's child list is indexed. Parent links let a
 * gap after a nested tool fall back to its containing assistant, preserving the
 * more precise anchor used when an input is trimmed inside a surviving round.
 */
export class TranscriptAnchorIndex {
    private rounds: Element[] = [];
    private dirty = true;
    private children = new WeakMap<Element, ChildIndex>();
    private readonly content: Element;

    constructor(content: Element) {
        this.content = content;
    }

    /** Rebuild only changed membership; unrelated mutations within other rounds stay local. */
    changed(records: readonly MutationRecord[]): void {
        for (const record of records) {
            const element = record.target.nodeType === 1
                ? record.target as Element : record.target.parentElement;
            const round = element?.closest(ROUND_SELECTOR);
            if (round && this.content.contains(round)) this.children.delete(round);
            else {
                this.dirty = true;
                // Release removed DOM even if the reader follows forever and
                // therefore never asks for the next membership rebuild.
                this.rounds = [];
            }
        }
    }

    private childIndex(round: Element): ChildIndex {
        let index = this.children.get(round);
        if (!index) {
            profileCount('scrollChildIndex');
            const elements = Array.from(round.querySelectorAll(CHILD_SELECTOR));
            const parents = new Map<Element, Element>();
            for (const element of elements) {
                const parent = element.parentElement?.closest(CHILD_SELECTOR);
                parents.set(element, parent && round.contains(parent) ? parent : round);
            }
            index = { elements, parents };
            this.children.set(round, index);
        }
        return index;
    }

    /** Layout boxes exclude entrance translations; no cached coordinates can become stale. */
    find(edge: number, bounds: (element: Element) => { top: number; bottom: number }): Element | undefined {
        if (this.dirty) {
            profileCount('scrollRoundIndex');
            this.rounds = Array.from(this.content.querySelectorAll(ROUND_SELECTOR));
            this.dirty = false;
        }
        const at = firstEndingAfter(this.rounds, edge, element => bounds(element).bottom);
        const round = this.rounds[at];
        if (!round) return undefined;
        const children = this.childIndex(round);
        const child = lastStartingBefore(children.elements, edge, element => bounds(element).top);
        let candidate = children.elements[child];
        while (candidate && candidate !== round) {
            const box = bounds(candidate);
            if (box.top <= edge && box.bottom > edge) return candidate;
            candidate = children.parents.get(candidate);
        }
        return round;
    }
}
