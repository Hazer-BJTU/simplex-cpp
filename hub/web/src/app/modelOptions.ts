/** Model options advertised by the active worker, separate from local selections. */
export interface ModelOption {
    name: string;
    options: readonly unknown[];
}

export function modelOptionFields(data: unknown): readonly ModelOption[] {
    const model = (data as { model?: { available?: unknown } } | null)?.model;
    if (!Array.isArray(model?.available)) return [];
    return model.available.filter((item): item is ModelOption =>
        item !== null && typeof item === 'object'
        && typeof item.name === 'string' && Array.isArray(item.options));
}

export function currentModelOptions(data: unknown): Readonly<Record<string, unknown>> {
    const current = (data as { model?: { current?: unknown } } | null)?.model?.current;
    return current !== null && typeof current === 'object' && !Array.isArray(current)
        ? current as Record<string, unknown> : {};
}
