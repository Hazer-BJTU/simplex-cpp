import { memo, useState } from 'react';

/** Literal reasoning, lazily mounted. Expansion never invokes Markdown or highlighting. */
export const Reasoning = memo(function Reasoning({ text, truncated = false, bytes }: {
    text: string; truncated?: boolean; bytes?: number;
}) {
    const [open, setOpen] = useState(false);
    return <details className="min-w-0 rounded border border-line bg-sunken px-2 py-1"
        onToggle={event => setOpen(event.currentTarget.open)}>
        <summary className="cursor-pointer select-none text-xs text-ink-muted">reasoning</summary>
        {open && <div className="mt-1 text-sm text-ink-muted">
            <p data-testid="reasoning-text" className="whitespace-pre-wrap break-words [overflow-wrap:anywhere]">{text}</p>
            {truncated && <p className="text-xs text-ink-faint">Reasoning preview{bytes === undefined ? '' : ` of ${bytes} UTF-8 bytes`}; remaining text omitted.</p>}
        </div>}
    </details>;
});
