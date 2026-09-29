import { usePanel } from '../state/usePanel.ts';
import { Markdown } from './Markdown.tsx';

/** Independent current plan, restored on subscription rather than transcript replay. */
export function PlanCard() {
    const selected = usePanel((state) => state.selected);
    const plan = usePanel((state) => selected ? state.plans.get(selected) : undefined);
    if (!plan?.markdown.trim()) return null;
    return (
        <details key={selected} open className="plan-card mx-3 mt-2 min-h-0 shrink-0 rounded-lg border border-line bg-surface"
            data-testid="plan-card">
            <summary className="cursor-pointer px-3 py-2 text-xs font-medium text-ink-muted">Plan</summary>
            <div className="max-h-48 overflow-auto break-words px-3 pb-3 text-sm" data-testid="plan-content">
                <Markdown>{plan.markdown}</Markdown>
            </div>
        </details>
    );
}
