/**
 * @file empty states and skeletons.
 *
 * Two shapes the old panel did not have, and the difference between them is the
 * reason both exist:
 *
 *   - a **skeleton** says "this is coming and here is how much of it". It is the
 *     honest answer while a request is in flight, and it is what stops a panel
 *     from claiming a transcript is empty before it has heard one.
 *   - an **empty state** says "there is nothing here, and here is what would put
 *     something here". The old panel's version of both was one line of grey text
 *     — sometimes the wrong one.
 *
 * Neither is decorative. A loading transcript that says "Nothing yet" is a lie
 * the operator has no way to distinguish from the truth, which is the same
 * failure mode as a disabled button with no reason attached.
 */
import type { ReactNode } from 'react';
import { Glyph, type GlyphName } from './icons.tsx';

/** One grey bar standing in for a line of text that has not arrived. */
export function Skeleton({ className = '' }: { className?: string }) {
    return <span aria-hidden className={`skeleton block h-3 ${className}`} />;
}

/**
 * A block of skeleton lines.
 *
 * `role="status"` with a label rather than a visual-only shimmer: a screen
 * reader is told that something is loading, which is the part a shape cannot
 * communicate.
 */
export function LoadingLines({ label, lines = 3, className = '' }: {
    label: string;
    lines?: number;
    className?: string;
}) {
    const widths = ['w-3/4', 'w-full', 'w-5/6', 'w-2/3', 'w-11/12'];
    return (
        <div role="status" aria-busy="true" aria-label={label} className={`space-y-2 ${className}`}>
            {Array.from({ length: lines }, (_, index) => (
                <Skeleton key={index} className={widths[index % widths.length] ?? 'w-full'} />
            ))}
        </div>
    );
}

/** Nothing here, why, and what to do about it. */
export function EmptyState({ icon, title, detail, action, className = '' }: {
    icon: GlyphName;
    title: string;
    detail?: ReactNode;
    action?: ReactNode;
    className?: string;
}) {
    return (
        <div className={`grid place-items-center p-6 text-center ${className}`}>
            <div className="max-w-sm">
                <Glyph name={icon} size="lg" className="mx-auto text-ink-faint" />
                <p className="mt-2 text-sm font-medium text-ink">{title}</p>
                {detail && <p className="mt-1 text-xs text-ink-muted">{detail}</p>}
                {action && <div className="mt-3">{action}</div>}
            </div>
        </div>
    );
}
