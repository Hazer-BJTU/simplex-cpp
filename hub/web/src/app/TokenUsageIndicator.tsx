import { formatCacheRate, formatTokens, tokenUsageBand, type TokenUsage } from '../state/tokenUsage.ts';

/** Compact numeric usage shown above the composer. */
export function TokenUsageIndicator({ usage }: { usage: TokenUsage }) {
    return (
        <div aria-label="Latest token usage"
            className="reading-width mb-1 px-1 text-[10px] text-ink-muted tabular-nums">
            <div className="flex justify-end gap-2">
                <span>prompt {formatTokens(usage.prompt)}</span>
                <span>generated {formatTokens(usage.generated)}</span>
                <span>cache-rate {formatCacheRate(usage)}</span>
            </div>
        </div>
    );
}

/** Fixed-size header meter; its scale is independent of the provider limit. */
export function TokenUsageMeter({ usage }: { usage: TokenUsage }) {
    const band = tokenUsageBand(usage);
    const description = `Latest request: ${band.total.toLocaleString('en-US')} tokens. `
        + `Display level ${band.level} of 8. `
        + 'Each segment is 131,072 tokens (128K); the scale caps at 1,048,576 tokens (1M).';
    return (
        <div aria-label="Token usage band" title={description}
            className="flex shrink-0 items-center gap-1 text-[10px] text-ink-muted tabular-nums">
            <div role="meter" aria-label="Latest request token size"
                aria-valuemin={0} aria-valuemax={band.maximum}
                aria-valuenow={band.capped} aria-valuetext={description}
                className="flex h-1.5 w-20 gap-0.5 sm:w-32">
                {band.fills.map((fill, index) => (
                    <span key={index} className="h-full min-w-0 flex-1 overflow-hidden rounded-sm bg-line-strong">
                        <span data-testid="token-band-fill" className="block h-full bg-info"
                            style={{ width: `${fill * 100}%` }} />
                    </span>
                ))}
            </div>
            <span className="w-14 text-right">{band.level}/8 · {band.upper}</span>
        </div>
    );
}
