import { useCallback, useLayoutEffect, useRef, useState, type RefObject } from 'react';
import { profileCount } from '../lib/profile.ts';
import { ScrollOwnership, TranscriptAnchorIndex } from './scrollAnchors.ts';

const BOTTOM_DISTANCE = 48;

/**
 * One scroll owner. Resize/content growth never changes reader intent; wheel,
 * touch, keys and scrollbar drags do. A preserved DOM anchor keeps history
 * readers stationary when earlier content or approvals resize.
 *
 * The write happens where the browser still has time to paint it: in the
 * commit's layout effect, in the mutation and resize callbacks, and on mount.
 * Deferring it to the next animation frame painted one frame of the new
 * content below the fold — and one frame of the top of the conversation on
 * every session switch. The frame queue is kept only for scroll echoes, which
 * must not write from inside a scroll event.
 */
export function useTranscriptScroll(scroller: RefObject<HTMLDivElement | null>, active: boolean) {
    const [following, setFollowing] = useState(true);
    const follows = useRef(true);
    const land = useRef<() => void>(() => {});
    const resume = useRef<() => void>(() => {});

    useLayoutEffect(() => {
        const node = scroller.current;
        const content = node?.firstElementChild;
        if (!node || !content || !active) return;
        let frame = 0;
        let disposed = false;
        const ownership = new ScrollOwnership();
        const index = new TranscriptAnchorIndex(content);
        let anchor: { element: Element; offset: number } | undefined;

        function geometry() {
            return {
                height: node!.scrollHeight,
                viewport: node!.clientHeight,
                width: node!.clientWidth,
            };
        }
        function bounds(element: Element, translations: Map<Element, number>) {
            profileCount('scrollProbe');
            let translation = 0;
            for (let ancestor: Element | null = element; ancestor && ancestor !== node; ancestor = ancestor.parentElement) {
                let value = translations.get(ancestor);
                if (value === undefined) {
                    const transform = getComputedStyle(ancestor).transform;
                    value = transform === 'none' ? 0 : new DOMMatrixReadOnly(transform).m42;
                    translations.set(ancestor, value);
                }
                translation += value;
            }
            // Entrance transforms do not change layout and must not be treated
            // as history growth that needs compensating scroll writes.
            const box = element.getBoundingClientRect();
            return { top: box.top - translation, bottom: box.bottom - translation };
        }
        function offset(element: Element): number {
            return bounds(element, new Map()).top - node!.getBoundingClientRect().top;
        }
        function capture() {
            if (follows.current) {
                anchor = undefined;
                return;
            }
            profileCount('scrollCapture');
            index.changed(mutation.takeRecords());
            const top = node!.getBoundingClientRect().top;
            const translations = new Map<Element, number>();
            const element = index.find(top, element => bounds(element, translations));
            anchor = element ? { element, offset: bounds(element, translations).top - top } : undefined;
        }
        function flush() {
            if (frame) {
                cancelAnimationFrame(frame);
                frame = 0;
            }
            if (disposed || !node!.clientHeight) return;
            // The browser can move the position for the reader before the scroll
            // event that reports it arrives. Writing first would undo that move,
            // so the same ownership test the scroll handler uses runs here too.
            // The anchor from that movement stays the baseline: this commit's
            // layout shift is still compensated against it below.
            if (ownership.readerMoved(node!.scrollTop, performance.now(), geometry())) {
                const distance = node!.scrollHeight - node!.scrollTop - node!.clientHeight;
                const value = distance <= BOTTOM_DISTANCE;
                follows.current = value;
                setFollowing(value);
            }
            if (follows.current) {
                node!.scrollTop = node!.scrollHeight - node!.clientHeight;
            } else if (anchor?.element.isConnected) {
                const currentOffset = offset(anchor.element);
                node!.scrollTop += currentOffset - anchor.offset;
            }
            // Read after writing: scrollTop may have been clamped or rounded.
            ownership.wrote(node!.scrollTop, geometry());
            capture();
        }
        /** Only for writes that must not run inside a scroll event. */
        function defer() {
            if (!disposed && !frame) frame = requestAnimationFrame(flush);
        }
        function intent() {
            ownership.intent(performance.now(), node!.scrollTop, geometry());
        }
        function scroll() {
            const dimensions = geometry();
            const distance = dimensions.height - node!.scrollTop - dimensions.viewport;
            if (ownership.readerMoved(node!.scrollTop, performance.now(), dimensions)) {
                const value = distance <= BOTTOM_DISTANCE;
                follows.current = value;
                setFollowing(value);
                capture();
            } else if (follows.current && distance > 0.5) {
                // A superseded PageUp animation or automatic movement must
                // not leave explicit following stranded above the latest item.
                defer();
            }
        }
        land.current = flush;
        resume.current = () => {
            ownership.resumeFollowing();
            flush();
        };
        const mutation = new MutationObserver(records => {
            index.changed(records);
            flush();
        });
        mutation.observe(content, { childList: true, subtree: true });
        const resize = new ResizeObserver(flush);
        resize.observe(node);
        resize.observe(content);
        node.addEventListener('scroll', scroll, { passive: true });
        for (const event of ['wheel', 'touchstart', 'pointerdown', 'keydown']) {
            node.addEventListener(event, intent, { passive: true });
        }
        // Returning from a hidden pane preserves the browser's scroll offset.
        // Read the anchor before any scheduled follow/resize work.
        capture();
        flush();
        return () => {
            disposed = true;
            cancelAnimationFrame(frame);
            mutation.disconnect();
            resize.disconnect();
            node.removeEventListener('scroll', scroll);
            for (const event of ['wheel', 'touchstart', 'pointerdown', 'keydown']) node.removeEventListener(event, intent);
            land.current = () => {};
            resume.current = () => {};
        };
    }, [active, scroller]);

    // Runs after a coherent React commit, not once for each ingested event.
    useLayoutEffect(() => { if (active) land.current(); });
    const jumpToLatest = useCallback(() => {
        follows.current = true;
        setFollowing(true);
        resume.current();
    }, []);
    return { following, jumpToLatest };
}
