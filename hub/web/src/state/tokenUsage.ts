/** Usage of one model response; cache-hit tokens are included in prompt tokens. */
export interface TokenUsage {
    readonly prompt: number;
    readonly generated: number;
    readonly cacheHit: number;
}

/** Ignore absent/malformed costs without erasing the last reported usage. */
export function parseTokenUsage(value: unknown): TokenUsage | null {
    if (value === null || typeof value !== 'object' || Array.isArray(value)) return null;
    const cost = value as Record<string, unknown>;
    const fields = ['prompt', 'generated', 'cache_hit'] as const;
    if (!fields.some((field) => cost[field] !== undefined)) return null;
    if (fields.some((field) => cost[field] !== undefined
        && (typeof cost[field] !== 'number' || !Number.isFinite(cost[field])
            || (cost[field] as number) < 0))) return null;
    return {
        prompt: (cost.prompt as number | undefined) ?? 0,
        generated: (cost.generated as number | undefined) ?? 0,
        cacheHit: (cost.cache_hit as number | undefined) ?? 0,
    };
}

/** Decimal SI units, with one fractional digit even for zero or whole values. */
export function formatTokens(value: number): string {
    for (const [scale, unit] of [[1e9, 'B'], [1e6, 'M'], [1e3, 'K']] as const) {
        if (value >= scale) return `${(value / scale).toFixed(1)}${unit}`;
    }
    return value.toFixed(1);
}

export function formatCacheRate(usage: TokenUsage): string {
    const ratio = usage.prompt > 0 ? Math.min(1, usage.cacheHit / usage.prompt) : 0;
    return `${(ratio * 100).toFixed(1)}%`;
}

/** Context-window scale: eight 128 Ki-token bands, capped at 1 Mi-token. */
export function tokenUsageBand(usage: TokenUsage) {
    const bandSize = 128 * 1024;
    const maximum = bandSize * 8;
    const total = usage.prompt + usage.generated;
    const capped = Math.min(total, maximum);
    const level = Math.max(1, Math.ceil(capped / bandSize));
    return {
        total,
        capped,
        maximum,
        level,
        upper: level === 8 ? '1M' : `${level * 128}K`,
        fills: Array.from({ length: 8 }, (_, index) =>
            Math.max(0, Math.min(1, (capped - index * bandSize) / bandSize))),
    };
}
