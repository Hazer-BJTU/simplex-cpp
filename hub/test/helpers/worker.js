/**
 * @file test helper: a stand-in worker built on the `ws` client.
 *
 * The real worker is a C++ process; unit and integration tests need a scriptable
 * peer that can speak the same envelopes, arrive late, misbehave on purpose, or
 * disappear mid-exchange. This helper is that peer and nothing more.
 */
import { WebSocket } from 'ws';

/**
 * Open a worker-style connection.
 *
 * @param {string} url upgrade URL including any token query.
 * @param {object} [options]
 * @param {number} [options.timeout] connect timeout in milliseconds.
 * @returns {Promise<object>} connection facade with `ws`, `messages`,
 *   `send`, `waitFor`, `waitForClose`, and `close`.
 */
export async function connectWorker(url, { timeout = 3000 } = {}) {
    const ws = new WebSocket(url);
    const messages = [];
    const waiters = new Set();
    const closeWaiters = new Set();
    let closed = null;

    const settle = () => {
        for (const waiter of [...waiters]) {
            const found = messages.find(waiter.predicate);
            if (found !== undefined) {
                waiters.delete(waiter);
                clearTimeout(waiter.timer);
                waiter.resolve(found);
            }
        }
        if (closed !== null) {
            for (const waiter of [...closeWaiters]) {
                closeWaiters.delete(waiter);
                clearTimeout(waiter.timer);
                waiter.resolve(closed);
            }
        }
    };

    ws.on('message', (data) => {
        messages.push(JSON.parse(data.toString('utf8')));
        settle();
    });
    ws.on('close', (code, reason) => {
        closed = { code, reason: reason?.toString('utf8') ?? '' };
        settle();
    });

    await new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error(`connect timeout for ${url}`)), timeout);
        const fail = (error) => {
            clearTimeout(timer);
            reject(error);
        };
        ws.once('open', () => {
            clearTimeout(timer);
            resolve();
        });
        ws.once('error', fail);
        ws.once('unexpected-response', (_request, response) => {
            fail(Object.assign(new Error(`upgrade failed with ${response.statusCode}`), {
                statusCode: response.statusCode,
            }));
        });
    });

    return {
        ws,
        messages,
        get closed() {
            return closed;
        },
        /** Send a JSON message. */
        send(value) {
            ws.send(JSON.stringify(value));
        },
        /** Send raw bytes as a binary frame. */
        sendBinary(value) {
            ws.send(value, { binary: true });
        },
        /**
         * Resolve with the first message matching `predicate`, including
         * messages that already arrived.
         */
        waitFor(predicate, { timeout: waitTimeout = timeout, label = 'message' } = {}) {
            const found = messages.find(predicate);
            if (found !== undefined) return Promise.resolve(found);
            return new Promise((resolve, reject) => {
                const waiter = { predicate, resolve };
                waiter.timer = setTimeout(() => {
                    waiters.delete(waiter);
                    reject(new Error(`timed out waiting for ${label}; saw ${JSON.stringify(messages)}`));
                }, waitTimeout);
                waiter.timer.unref?.();
                waiters.add(waiter);
            });
        },
        /** Wait for the socket to close. */
        waitForClose({ timeout: closeTimeout = timeout } = {}) {
            if (closed !== null) return Promise.resolve(closed);
            return new Promise((resolve, reject) => {
                const waiter = { resolve };
                waiter.timer = setTimeout(() => {
                    closeWaiters.delete(waiter);
                    reject(new Error('timed out waiting for close'));
                }, closeTimeout);
                waiter.timer.unref?.();
                closeWaiters.add(waiter);
            });
        },
        /** Close the socket and wait for the handshake to finish. */
        async close() {
            if (ws.readyState === WebSocket.CLOSED) return closed;
            ws.close(1000, 'test done');
            return this.waitForClose();
        },
    };
}

/** Open a connection that is expected to be rejected during the upgrade. */
export async function upgradeStatus(url, { timeout = 3000 } = {}) {
    const ws = new WebSocket(url);
    try {
        return await new Promise((resolve, reject) => {
            const timer = setTimeout(() => reject(new Error('upgrade timeout')), timeout);
            ws.once('open', () => {
                clearTimeout(timer);
                resolve(101);
            });
            ws.once('error', () => {});
            ws.once('unexpected-response', (_request, response) => {
                clearTimeout(timer);
                response.resume();
                resolve(response.statusCode);
            });
        });
    } finally {
        ws.terminate();
    }
}

/** Build an event envelope with defaults for the fields tests do not vary. */
export function workerEvent({
    event = 'status',
    session = 'demo',
    worker = 'worker-1',
    sequence = 1,
    requestId = '',
    runId = '',
    data = {},
    extra = {},
} = {}) {
    return {
        type: 'event',
        event,
        session_id: session,
        worker_id: worker,
        request_id: requestId,
        run_id: runId,
        sequence,
        data,
        ...extra,
    };
}

/** Wait for a condition to become true, polling the microtask queue. */
export async function until(predicate, { timeout = 2000, label = 'condition' } = {}) {
    const deadline = Date.now() + timeout;
    for (;;) {
        // `await` handles both a synchronous predicate and an asynchronous one,
        // which the child-process end-to-end tests need.
        const value = await predicate();
        if (value) return value;
        if (Date.now() > deadline) throw new Error(`timed out waiting for ${label}`);
        await new Promise((resolve) => setTimeout(resolve, 5));
    }
}
