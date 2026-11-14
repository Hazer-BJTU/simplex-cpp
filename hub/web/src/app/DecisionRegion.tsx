/**
 * @file the region above the composer that asks the operator something.
 *
 * Approvals and `ask_user` questions are both decisions the run is waiting on,
 * so they share one place and now one height budget. The budget is the point:
 * with independent caps the two regions could each take 45dvh, and on a short
 * viewport their sum pushed the composer — the control the operator is about to
 * use — past the bottom of the window.
 *
 * The region scrolls as a whole rather than per section, so a scrollbar appears
 * once. Its own scrollbar narrows the content, so it publishes its width for the
 * children to subtract, keeping their edges on the conversation's measure.
 */
import { useLayoutEffect, useRef, type ReactNode } from 'react';

export function DecisionRegion({ children }: { children: ReactNode }) {
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
    return <div ref={region} className="decisions-shell">{children}</div>;
}
