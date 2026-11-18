import { useCallback, useLayoutEffect, useRef, useState, type ReactNode } from 'react';

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
    useLayoutEffect(() => {
        const node = ref.current;
        if (!leaving || !node) return;
        let current = true;
        void Promise.allSettled(node.getAnimations().map(animation => animation.finished)).then(() => {
            if (current) finishExit(entryKey);
        });
        return () => { current = false; };
    }, [entryKey, leaving, finishExit]);

    return (
        <div ref={ref} className="decision-motion" data-state={leaving ? 'leaving' : 'open'} inert={leaving}>
            {children}
        </div>
    );
}
