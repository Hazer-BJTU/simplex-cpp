/**
 * @file the React binding for the panel store.
 *
 * Kept apart from `store.ts` for one concrete reason: that file must stay
 * importable by `node --test`, and the React-bound `create` from `zustand`
 * pulls React in. Splitting them means the store's rules — replay merging,
 * session-list merging, epoch resets — are covered by tests that run in
 * milliseconds and without a browser, while components still get the
 * subscription behaviour.
 */
import { useStore } from 'zustand';
import { useCallback, useMemo, useRef, useSyncExternalStore } from 'react';
import { shallow } from 'zustand/vanilla/shallow';
import type { SessionDescription, SessionId } from '../../../shared/protocol.ts';
import { panelStore, type PanelStore } from './store.ts';
import type { ViewState } from './view.ts';

/**
 * Subscribe to one slice of the store.
 *
 * The selector must return a value that is stable between renders unless it
 * changed: `s => s.sessions.get(id)` is right, `s => ({ a: s.a })` is not,
 * because a new object every call re-renders forever. The store replaces the
 * containers it changes rather than mutating them, which is what makes the
 * cheap selectors correct — and why derived values such as the transcript
 * counters are pure functions of a view rather than store methods.
 */
export function usePanel<T>(selector: (state: PanelStore) => T): T {
    return useStore(panelStore, selector);
}

/**
 * Suspend visual subscriptions while a preserved pane is hidden. Ingestion
 * still updates the store; showing the pane reads the current snapshot at once.
 * Shallow equality keeps unrelated fields from scheduling a visual render.
 */
export function useVisiblePanel<T>(active: boolean, selector: (state: PanelStore) => T): T {
    const previous = useRef<{ value: T } | undefined>(undefined);
    const snapshot = useMemo(() => () => {
        if (!active && previous.current) return previous.current.value;
        const value = selector(panelStore.getState());
        if (!previous.current || !shallow(previous.current.value, value)) previous.current = { value };
        return previous.current.value;
    }, [active, selector]);
    const subscribe = useCallback((notify: () => void) => active
        ? panelStore.subscribe(notify) : () => {}, [active]);
    return useSyncExternalStore(subscribe, snapshot, snapshot);
}

/** One session's view, or undefined. Stable identity, safe as a selector. */
export function useView(sessionId: SessionId | null): ViewState | undefined {
    return usePanel((state) => (sessionId ? state.views.get(sessionId) : undefined));
}

/** One session's description, or null. */
export function useSession(sessionId: SessionId | null): SessionDescription | null {
    return usePanel((state) => (sessionId ? state.sessions.get(sessionId) ?? null : null));
}

export { panelStore };
export type { PanelStore };
