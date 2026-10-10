/** A fixed coalescing window for reconstructible projections, never ownership or receipts. */
export const PROJECTION_FLUSH_MS = 200;

/**
 * Keep one pending callback, reading the latest owner state at flush time.
 * Later updates do not move the deadline, so continuous traffic cannot starve
 * persistence. Tasks are synchronous: stop() cancels the timer and completes
 * the last attempt before returning. Failure reporting cannot escape a timer.
 */
export class ProjectionFlush {
    private timer: NodeJS.Timeout | null = null;
    private dirty = false;
    private stopped = false;
    private readonly task: () => void;
    private readonly failed: (error: unknown) => void;

    constructor(task: () => void, failed: (error: unknown) => void) {
        this.task = task;
        this.failed = failed;
    }

    schedule(): void {
        if (this.stopped) return;
        this.dirty = true;
        if (this.timer) return;
        this.timer = setTimeout(() => this.flush(), PROJECTION_FLUSH_MS);
        this.timer.unref();
    }

    /** Supersede a pending projection with a synchronous authoritative write. */
    cancel(): void {
        if (this.timer) clearTimeout(this.timer);
        this.timer = null;
        this.dirty = false;
    }

    /** Attempt the pending write once; later changes can retry a failed projection. */
    flush(): boolean {
        const dirty = this.dirty;
        this.cancel();
        if (!dirty) return true;
        try {
            this.task();
            return true;
        } catch (error) {
            try { this.failed(error); } catch { /* observers cannot fail a timer or cleanup */ }
            return false;
        }
    }

    /** Reject future scheduling before flushing; no callback outlives this boundary. */
    stop(): boolean {
        this.stopped = true;
        return this.flush();
    }
}
