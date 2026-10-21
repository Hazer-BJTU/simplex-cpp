/**
 * @file one blocking error, with one place for its details.
 *
 * A run can fail because the model request failed, because the worker stopped
 * after a storage error, or because a compaction attempt did not finish. They
 * arrive from different events but ask the operator the same three questions:
 * what failed, what is still safe, and what can be done now. This component
 * answers those once, so the transcript does not grow a second red box for
 * every event that happened to carry an error string.
 *
 * The raw diagnostic is deliberately behind a disclosure and copies verbatim.
 * Provider errors are often JSON, HTML, or a multi-line stack, and a panel that
 * reformats them makes support harder rather than easier.
 */
import type { ReactNode } from 'react';
import { Glyph, type GlyphName } from '../ui/icons.tsx';
import { CopyButton } from './Markdown.tsx';

/** How loud the notice is. Nonterminal diagnostics use `warn`. */
export type ErrorTone = 'danger' | 'warn' | 'neutral';

const TONES: Record<ErrorTone, { frame: string; icon: GlyphName }> = {
    danger: { frame: 'border-danger-line bg-danger-soft text-danger', icon: 'error' },
    warn: { frame: 'border-warn-line bg-warn-soft text-warn', icon: 'warning' },
    neutral: { frame: 'border-line bg-sunken text-ink-muted', icon: 'info' },
};

export function ErrorNotice({ tone = 'danger', title, message, meta, details, actions,
    role, testId, className = '' }: {
    tone?: ErrorTone;
    title: string;
    /** The sentence that says what the operator can do now. */
    message?: ReactNode;
    /** Stage, operation, or retryability: small facts, not a second sentence. */
    meta?: ReactNode;
    /** Raw diagnostic text; the disclosure is omitted when this is empty. */
    details?: string | undefined;
    actions?: ReactNode;
    /** `alert` only for the current run; historical errors must not interrupt. */
    role?: 'alert' | 'status' | undefined;
    testId?: string | undefined;
    className?: string;
}) {
    const shown = TONES[tone];
    const raw = details?.trim() ?? '';
    return (
        <section
            data-testid={testId}
            role={role}
            className={`rounded-lg border px-3 py-2 text-sm ${shown.frame} ${className}`}
        >
            <div className="flex items-start gap-2">
                <Glyph name={shown.icon} size="sm" className="mt-0.5 shrink-0" />
                <div className="min-w-0 flex-1">
                    <div className="flex flex-wrap items-baseline gap-x-2 gap-y-1">
                        <p className="font-medium">{title}</p>
                        {meta && <span className="text-xs font-normal">{meta}</span>}
                    </div>
                    {message && <p className="mt-1 leading-relaxed">{message}</p>}
                    {actions && <div className="mt-2 flex flex-wrap items-center gap-2">{actions}</div>}
                    {raw && (
                        <details className="mt-2">
                            <summary className="cursor-pointer text-xs">Error details</summary>
                            <div className="mt-1 flex justify-end">
                                <CopyButton text={() => raw} label="copy details" />
                            </div>
                            <pre className="max-h-64 overflow-auto whitespace-pre-wrap break-words
                                [overflow-wrap:anywhere] rounded border border-line bg-surface p-2
                                font-mono text-xs text-ink">{raw}</pre>
                        </details>
                    )}
                </div>
            </div>
        </section>
    );
}
