/** Opt-in, bounded counters for offline panel profiling; never retain content. */
const enabled = typeof window !== 'undefined'
    && new URLSearchParams(window.location.search).get('panel_profile') === '1';

interface Counter {
    count: number;
    milliseconds: number;
}

const counters: Record<string, Counter> = {};

/** Fixed operation names only. Normal panels do not sample the clock. */
export function profileCount(name: string, milliseconds = 0): void {
    if (!enabled) return;
    const counter = counters[name] ??= { count: 0, milliseconds: 0 };
    counter.count += 1;
    counter.milliseconds += milliseconds;
}

/** Time a synchronous derivation without changing its result or exceptions. */
export function profile<T>(name: string, operation: () => T): T {
    if (!enabled) return operation();
    const start = performance.now();
    try {
        return operation();
    } finally {
        profileCount(name, performance.now() - start);
    }
}

if (enabled) {
    Object.defineProperty(window, '__simplexPanelProfile', {
        value: {
            read: () => structuredClone(counters),
            reset: () => {
                for (const key of Object.keys(counters)) delete counters[key];
            },
        },
    });
}
