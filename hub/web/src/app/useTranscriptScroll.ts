import { useCallback, useLayoutEffect, useRef, useState, type RefObject } from 'react';

const BOTTOM_DISTANCE = 48;

/**
 * One frame-coalesced scroll owner. Resize/content growth never changes reader
 * intent; wheel, touch, keys and scrollbar drags do. A preserved DOM anchor
 * keeps history readers stationary when earlier content or approvals resize.
 */
export function useTranscriptScroll(scroller: RefObject<HTMLDivElement | null>, active: boolean) {
    const [following, setFollowing] = useState(true);
    const follows = useRef(true);
    const schedule = useRef<() => void>(() => {});

    useLayoutEffect(() => {
        const node = scroller.current;
        const content = node?.firstElementChild;
        if (!node || !content || !active) return;
        let frame = 0;
        let intentUntil = 0;
        let writing = false;
        let anchor: { element: Element; offset: number } | undefined;

        function offset(element: Element): number {
            let translation = 0;
            for (let ancestor: Element | null = element; ancestor && ancestor !== node; ancestor = ancestor.parentElement) {
                const transform = getComputedStyle(ancestor).transform;
                if (transform !== 'none') translation += new DOMMatrixReadOnly(transform).m42;
            }
            // Entrance transforms do not change layout and must not be treated
            // as history growth that needs compensating scroll writes.
            return element.getBoundingClientRect().top - node!.getBoundingClientRect().top - translation;
        }
        function capture() {
            if (follows.current) {
                anchor = undefined;
                return;
            }
            const top = node!.getBoundingClientRect().top;
            anchor = undefined;
            for (const element of content!.querySelectorAll('[data-testid="round"], [data-testid="history-turn"]')) {
                const bounds = element.getBoundingClientRect();
                if (bounds.bottom > top) {
                    // An outbox can be trimmed above the message currently
                    // being read within this same round. Prefer the visible
                    // message/tool crossing the viewport edge over its parent.
                    let visible = element;
                    for (const child of element.querySelectorAll(
                        '[data-testid="assistant-message"], [data-testid="tool-card"], [data-testid="outbox-item"]',
                    )) {
                        const childBounds = child.getBoundingClientRect();
                        if (childBounds.top <= top && childBounds.bottom > top && visible.contains(child)) visible = child;
                    }
                    anchor = { element: visible, offset: offset(visible) };
                    break;
                }
            }
        }
        function update() {
            frame = 0;
            if (!node!.clientHeight) return;
            writing = true;
            if (follows.current) {
                node!.scrollTop = node!.scrollHeight - node!.clientHeight;
            } else if (anchor?.element.isConnected) {
                const currentOffset = offset(anchor.element);
                node!.scrollTop += currentOffset - anchor.offset;
            }
            capture();
            // Scroll delivery is asynchronous. Clear writer ownership in the
            // next task, while genuine pointer/keyboard input can still win.
            queueMicrotask(() => { writing = false; });
        }
        function enqueue() {
            if (!frame) frame = requestAnimationFrame(update);
        }
        function intent() {
            intentUntil = performance.now() + 1000;
        }
        function scroll() {
            if (!writing && performance.now() < intentUntil) {
                const value = node!.scrollHeight - node!.scrollTop - node!.clientHeight <= BOTTOM_DISTANCE;
                follows.current = value;
                setFollowing(value);
                capture();
            }
        }
        schedule.current = enqueue;
        const resize = new ResizeObserver(enqueue);
        resize.observe(node);
        resize.observe(content);
        node.addEventListener('scroll', scroll, { passive: true });
        for (const event of ['wheel', 'touchstart', 'pointerdown', 'keydown']) {
            node.addEventListener(event, intent, { passive: true });
        }
        // Returning from a hidden pane preserves the browser's scroll offset.
        // Read the anchor before any scheduled follow/resize work.
        capture();
        enqueue();
        return () => {
            cancelAnimationFrame(frame);
            resize.disconnect();
            node.removeEventListener('scroll', scroll);
            for (const event of ['wheel', 'touchstart', 'pointerdown', 'keydown']) node.removeEventListener(event, intent);
            schedule.current = () => {};
        };
    }, [active, scroller]);

    // Runs after a coherent React commit, not once for each ingested event.
    useLayoutEffect(() => { if (active) schedule.current(); });
    const jumpToLatest = useCallback(() => {
        follows.current = true;
        setFollowing(true);
        schedule.current();
    }, []);
    return { following, jumpToLatest };
}
