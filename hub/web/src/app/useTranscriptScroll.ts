import { useCallback, useLayoutEffect, useRef, useState, type RefObject } from 'react';
import { profileCount } from '../lib/profile.ts';
import { ScrollOwnership, TranscriptAnchorIndex } from './scrollAnchors.ts';

const BOTTOM_DISTANCE = 48;
/** A line is 25.6px tall; growth shorter than this moves without an animation. */
const EASE_DISTANCE = 24;
/** Time constant of the follow glide: the visible movement ends in about 3τ. */
const FOLLOW_TAU = 45;
/** Smallest step the glide takes, so it lands in finite time. */
const FOLLOW_STEP = 0.75;
/**
 * Growth that follows the previous growth this closely is a replay or a burst,
 * not a message arriving: it lands instead of gliding. Hydration after a mount
 * or a session switch delivers many batches within a few frames.
 */
const GLIDE_QUIET_MS = 250;
/** Jump to latest: long enough to read as movement, short enough to feel direct. */
const JUMP_MIN_MS = 160;
const JUMP_MAX_MS = 280;
const JUMP_MS_PER_PX = 8;

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
 *
 * Movement is eased, position is not. A jump to the latest message and growth
 * larger than a line glide toward the end; a mount or a session switch lands
 * instantly, because arriving somewhere is not the same as moving there, and
 * replaying a thousand pixels of history on every switch would be a lie about
 * what happened. Shrinking content and layout changes land instantly too.
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
        /** The tween that owns the position, if any. Null means the browser does. */
        let animation: {
            kind: 'follow' | 'jump';
            from: number;
            started: number;
            duration: number;
            last: number;
        } | null = null;
        /** The first write after mount lands instantly: a switch is not a scroll. */
        let settled = false;
        let lastHeight = node.scrollHeight;
        let lastViewport = node.clientHeight;
        let lastGrowthAt = performance.now();
        const motion = window.matchMedia('(prefers-reduced-motion: reduce)');
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
        function write(top: number) {
            node!.scrollTop = top;
            // Read after writing: scrollTop may have been clamped or rounded.
            ownership.wrote(node!.scrollTop, geometry());
        }
        function cancelFrame() {
            if (frame) {
                cancelAnimationFrame(frame);
                frame = 0;
            }
        }
        function stop() {
            animation = null;
            cancelFrame();
        }
        function bottom(): number {
            return node!.scrollHeight - node!.clientHeight;
        }
        function schedule() {
            if (disposed || frame || !node!.clientHeight) return;
            frame = requestAnimationFrame(run);
        }
        /**
         * Land the viewport where the reader expects it. Content that grew may
         * glide; a layout change — a resized viewport, the decision region
         * opening — has no growth and lands instantly. The decision is made here
         * rather than by the caller because a content growth and a reflow of the
         * same element both arrive through the resize observer.
         */
        function flush() {
            if (disposed || !node!.clientHeight) {
                stop();
                return;
            }
            // The browser can move the position for the reader before the scroll
            // event that reports it arrives. Writing first would undo that move,
            // so the same ownership test the scroll handler uses runs here too.
            // The anchor from that movement stays the baseline: this commit's
            // layout shift is still compensated against it below.
            if (ownership.readerMoved(node!.scrollTop, performance.now(), geometry())) {
                const distance = bottom() - node!.scrollTop;
                const value = distance <= BOTTOM_DISTANCE;
                follows.current = value;
                setFollowing(value);
                if (!value) stop();
            }
            const height = node!.scrollHeight;
            const growth = height - lastHeight;
            lastHeight = height;
            const viewport = node!.clientHeight;
            const resized = viewport !== lastViewport;
            lastViewport = viewport;
            const now = performance.now();
            const quiet = now - lastGrowthAt > GLIDE_QUIET_MS;
            if (growth > 0.5) lastGrowthAt = now;
            if (!follows.current) {
                stop();
                if (anchor?.element.isConnected) {
                    write(node!.scrollTop + (offset(anchor.element) - anchor.offset));
                }
                capture();
                return;
            }
            // A running tween recomputes the bottom on every frame, so growth is
            // picked up without a write here, and its own step lands a shrink
            // that leaves the reader past the end. A viewport resize changes
            // what the reader is looking at and must land immediately.
            if (animation) {
                if (!resized) {
                    schedule();
                    return;
                }
                stop();
            }
            const aim = bottom();
            const delta = aim - node!.scrollTop;
            const glide = settled && quiet && growth > 0.5
                && delta > EASE_DISTANCE && !motion.matches;
            settled = true;
            if (glide) {
                animation = { kind: 'follow', from: node!.scrollTop,
                    started: performance.now(), duration: 0, last: performance.now() };
                schedule();
                return;
            }
            write(aim);
            capture();
        }
        /** One animation step. The target is re-read every frame, never cached. */
        function tick(now: number) {
            const current = animation;
            if (!current) return;
            if (disposed || !node!.clientHeight) {
                stop();
                return;
            }
            const aim = bottom();
            if (motion.matches) {
                stop();
                write(aim);
                capture();
                return;
            }
            if (current.kind === 'jump') {
                const progress = current.duration > 0
                    ? Math.min(1, (now - current.started) / current.duration) : 1;
                if (progress >= 1) {
                    stop();
                    write(aim);
                    capture();
                    return;
                }
                const eased = 1 - (1 - progress) ** 3;
                write(current.from + (aim - current.from) * eased);
                schedule();
                return;
            }
            const delta = aim - node!.scrollTop;
            if (delta <= 0 || Math.abs(delta) <= 0.5) {
                // Shrinking content is not a movement: it lands where it is.
                stop();
                write(aim);
                capture();
                return;
            }
            const elapsed = Math.min(64, Math.max(1, now - current.last));
            current.last = now;
            const step = delta * (1 - Math.exp(-elapsed / FOLLOW_TAU));
            write(node!.scrollTop + Math.max(step, FOLLOW_STEP));
            schedule();
        }
        function run(now: number) {
            frame = 0;
            if (animation) tick(now);
            else flush();
        }
        function intent() {
            ownership.intent(performance.now(), node!.scrollTop, geometry());
            // A gesture owns the position from here: nothing keeps moving under
            // the reader's hand. The scroll event decides whether following
            // survives, exactly as it did before there was anything to stop.
            stop();
        }
        function scroll() {
            const dimensions = geometry();
            const distance = dimensions.height - node!.scrollTop - dimensions.viewport;
            if (ownership.readerMoved(node!.scrollTop, performance.now(), dimensions)) {
                const value = distance <= BOTTOM_DISTANCE;
                follows.current = value;
                setFollowing(value);
                if (!value) stop();
                capture();
            } else if (follows.current && distance > 0.5) {
                // A superseded PageUp animation or automatic movement must
                // not leave explicit following stranded above the latest item.
                schedule();
            }
        }
        land.current = flush;
        resume.current = () => {
            ownership.resumeFollowing();
            follows.current = true;
            setFollowing(true);
            if (disposed || !node!.clientHeight) return;
            const aim = bottom();
            const delta = aim - node!.scrollTop;
            stop();
            if (motion.matches || Math.abs(delta) <= 0.5) {
                write(aim);
                capture();
                return;
            }
            animation = { kind: 'jump', from: node!.scrollTop, started: performance.now(),
                duration: Math.min(JUMP_MAX_MS, Math.max(JUMP_MIN_MS, Math.abs(delta) / JUMP_MS_PER_PX)),
                last: 0 };
            schedule();
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
            stop();
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
