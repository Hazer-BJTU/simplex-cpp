/**
 * @file connection status, as a dot and a word.
 *
 * The panel used to draw a connection as a tinted pill with an inset ring. At
 * 12px beside a row of controls that reads as another button, and the ring is
 * the part that makes it feel boxed in. A status is ambient information rather
 * than an action, so the tone moves to a small dot and the meaning stays in the
 * word: an operator who cannot tell green from amber still reads "connected",
 * and the label does not depend on colour alone.
 *
 * `Badge` is still the right shape for something that asks for a decision, such
 * as a count of approvals. This component is for states that are simply true.
 */
import type { ReactNode } from 'react';

/** The meanings a status can carry, named after the job rather than a colour. */
export type StatusTone = 'ok' | 'info' | 'warn' | 'bad' | 'idle';

const DOT: Record<StatusTone, string> = {
    ok: 'bg-ok',
    info: 'bg-info',
    warn: 'bg-warn',
    bad: 'bg-danger',
    idle: 'bg-ink-faint',
};

const TEXT: Record<StatusTone, string> = {
    ok: 'text-ink-muted',
    info: 'text-ink-muted',
    warn: 'text-warn',
    bad: 'text-danger',
    idle: 'text-ink-muted',
};

export function Status({ tone = 'idle', pulse = false, children }: {
    tone?: StatusTone;
    /** For a state that is still settling, such as reconnecting. */
    pulse?: boolean;
    children: ReactNode;
}) {
    return (
        <span className={`inline-flex shrink-0 items-center gap-1.5 whitespace-nowrap
            text-xs font-medium ${TEXT[tone]}`}>
            <span
                aria-hidden="true"
                className={`h-1.5 w-1.5 shrink-0 rounded-full ${DOT[tone]}`
                    + `${pulse ? ' animate-pulse-soft' : ''}`}
            />
            {children}
        </span>
    );
}
