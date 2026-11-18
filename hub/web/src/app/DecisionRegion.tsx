import { useCallback, useEffect, useLayoutEffect, useRef, useState, type ReactNode } from 'react';

/** Move focus to the visible reading pane without changing its scroll position. */
function focusReadingSurface(): void {
    for (const surface of document.querySelectorAll<HTMLElement>('[data-decision-fallback]')) {
        if (surface.getClientRects().length === 0) continue;
        surface.focus({ preventScroll: true });
        return;
    }
}

const FOCUSABLE = 'a[href], button:not([disabled]), textarea:not([disabled]), input:not([disabled]), '
    + 'select:not([disabled]), summary, [tabindex]:not([tabindex="-1"])';

/** Float decisions inside the active pane, below its tabs. The transcript's
 * viewport and the composer stay fixed while the independent list scrolls.
 * Headless sessions have no reading pane and keep approvals below their info. */
export function DecisionRegion({ children, inline = false }: { children: ReactNode; inline?: boolean }) {
    const region = useRef<HTMLDivElement>(null);
    useLayoutEffect(() => {
        const node = region.current;
        if (!node) return;
        const update = () => node.style.setProperty(
            '--decision-scrollbar', `${node.offsetWidth - node.clientWidth}px`,
        );
        update();
        const observer = new ResizeObserver(update);
        observer.observe(node);
        return () => observer.disconnect();
    }, []);
    useLayoutEffect(() => {
        const node = region.current;
        if (!node) return;
        const openCards = (): Element[] =>
            Array.from(node.querySelectorAll('.decision-motion[data-state="open"]'));
        /** True only for a focusable descendant of a reading pane that an open card covers. */
        const covered = (element: Element, cards = openCards()): boolean => {
            if (element.hasAttribute('data-decision-fallback')) return false;
            if (!element.closest('[data-decision-surface]')) return false;
            const target = element.getBoundingClientRect();
            // A card's content box can extend beyond the independently scrolling
            // decision shell. Only the clipped, visible part can hide a control.
            const viewport = node.getBoundingClientRect();
            return cards.some((card) => {
                const box = card.getBoundingClientRect();
                const top = Math.max(box.top, viewport.top);
                const bottom = Math.min(box.bottom, viewport.bottom);
                const left = Math.max(box.left, viewport.left);
                const right = Math.min(box.right, viewport.right);
                if (top >= bottom || left >= right) return false;
                return target.bottom > top && target.top < bottom
                    && target.right > left && target.left < right;
            });
        };
        const keepVisible = (): void => {
            const cards = openCards();
            if (cards.length === 0) return;
            const active = document.activeElement;
            if (active instanceof HTMLElement && active !== document.body && covered(active, cards)) {
                focusReadingSurface();
            }
        };
        const onFocus = (event: FocusEvent): void => {
            const target = event.target;
            if (target instanceof HTMLElement && covered(target)) focusReadingSurface();
        };
        const onKey = (event: KeyboardEvent): void => {
            if (event.key !== 'Tab') return;
            const active = document.activeElement;
            if (!(active instanceof HTMLElement) || !active.hasAttribute('data-decision-fallback')) return;
            const cards = openCards();
            if (cards.length === 0) return;
            const candidates = [...document.querySelectorAll<HTMLElement>(FOCUSABLE)]
                .filter(element => element !== active && element.getClientRects().length > 0
                    && !element.closest('[inert]') && !covered(element, cards));
            const later = candidates.find(element =>
                active.compareDocumentPosition(element) & Node.DOCUMENT_POSITION_FOLLOWING);
            const earlier = candidates.filter(element =>
                element.compareDocumentPosition(active) & Node.DOCUMENT_POSITION_FOLLOWING).at(-1);
            const target = event.shiftKey ? earlier : later;
            // Never fall back into a covered control: that would bounce focus
            // straight back to this reading surface.
            event.preventDefault();
            target?.focus();
        };
        document.addEventListener('focusin', onFocus, true);
        document.addEventListener('keydown', onKey, true);
        // Focus can become covered without a child-list mutation: the reader
        // scrolls a pane, the decision list scrolls, or the viewport resizes.
        document.addEventListener('scroll', keepVisible, true);
        window.addEventListener('resize', keepVisible);
        const resize = new ResizeObserver(keepVisible);
        resize.observe(node);
        for (const surface of document.querySelectorAll('[data-decision-surface]')) {
            resize.observe(surface);
        }
        const observer = new MutationObserver(keepVisible);
        observer.observe(node, { childList: true, subtree: true });
        keepVisible();
        return () => {
            document.removeEventListener('focusin', onFocus, true);
            document.removeEventListener('keydown', onKey, true);
            document.removeEventListener('scroll', keepVisible, true);
            window.removeEventListener('resize', keepVisible);
            resize.disconnect();
            observer.disconnect();
        };
    }, []);
    return (
        <div className={`decisions-layer${inline ? ' decisions-inline' : ''}`}>
            <div className="reading-width h-full">
                <div ref={region} className="decisions-shell">{children}</div>
            </div>
        </div>
    );
}

interface DecisionEntry<T> {
    key: string;
    item: T;
    leaving: boolean;
}

/** Render the current decisions plus the brief visual exits of removed cards.
 * The pending items remain the source of truth. Retention keeps card identity
 * stable for disclosure, draft answers and countdown updates. */
export function useDecisionEntries<T>(items: readonly T[], keyOf: (item: T) => string) {
    const [list, setList] = useState(() => ({
        items,
        entries: items.map(item => ({ key: keyOf(item), item, leaving: false })),
    }));
    if (list.items !== items) {
        const entries: DecisionEntry<T>[] = items.map(item => ({ key: keyOf(item), item, leaving: false }));
        const keys = new Set(entries.map(entry => entry.key));
        list.entries.forEach((entry, index) => {
            if (!keys.has(entry.key)) entries.splice(index, 0, { ...entry, leaving: true });
        });
        setList({ items, entries });
    }
    const finishExit = useCallback((key: string) => {
        setList(current => ({
            ...current,
            entries: current.entries.filter(entry => entry.key !== key || !entry.leaving),
        }));
    }, []);
    return { entries: list.entries, finishExit };
}

/** Exiting controls become inert in the same commit that starts their motion.
 * Wait for the actual CSS animation so reduced motion and cancellation also
 * release the retained card. */
export function DecisionMotion({ entryKey, leaving, finishExit, children }: {
    entryKey: string;
    leaving: boolean;
    finishExit: (key: string) => void;
    children: ReactNode;
}) {
    const ref = useRef<HTMLDivElement>(null);
    useEffect(() => {
        const node = ref.current;
        return () => {
            if (node?.contains(document.activeElement)) focusReadingSurface();
        };
    }, []);
    useLayoutEffect(() => {
        const node = ref.current;
        if (!leaving || !node) return;
        // A settled card no longer owns the reading surface. Move focus out
        // before the exit motion starts; never focus an approval action.
        if (node.contains(document.activeElement)) focusReadingSurface();
        let current = true;
        void Promise.allSettled(node.getAnimations().map(animation => animation.finished)).then(() => {
            if (current) finishExit(entryKey);
        });
        return () => {
            current = false;
            if (node.contains(document.activeElement)) focusReadingSurface();
        };
    }, [entryKey, leaving, finishExit]);

    return (
        <div ref={ref} className="decision-motion" data-state={leaving ? 'leaving' : 'open'} inert={leaving}>
            {children}
        </div>
    );
}
