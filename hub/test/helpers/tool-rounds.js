/** Deterministic transcript and scan counters shared by tool-index regressions and benchmarks. */
export function toolEvent(sequence, event, data, extra = {}) {
    return { kind: 'event', id: `event-${sequence}`, epoch: 'epoch', envelope: {
        type: 'event', event, data, session_id: 'demo', worker_id: 'worker',
        request_id: 'request', run_id: 'run', sequence, hub_sequence: sequence,
        received_at: new Date(1700000000000 + sequence * 100).toISOString(), ...extra,
    } };
}

export function toolCall(id, name = 'run_command', args = {}) {
    return { id, name, arguments: args, security: 'RequireConfirm', type: 'parall_write' };
}

export function toolResult(query, text) {
    return { query: { ...query, security: 'Trusted' }, output: { type: 'text', raw: text } };
}

/** Multiple exchanges in one run, reversed results, and matching omission notices. */
export function toolTranscript(exchanges = 32, batchSize = 32) {
    const items = [toolEvent(1, 'input_admitted', { operation: 'message' })];
    const omission = { display_omitted: true, omitted_items: 36 };
    let sequence = 2;
    for (let exchange = 0; exchange < exchanges; exchange += 1) {
        const calls = Array.from({ length: batchSize }, (_, ordinal) =>
            toolCall(`call-${exchange}-${ordinal}`, `tool-${ordinal % 4}`, { exchange, ordinal }));
        items.push(toolEvent(sequence++, 'model_response', { invokes: [...calls, omission] }),
            toolEvent(sequence++, 'tool_calls', [...calls, omission]),
            toolEvent(sequence++, 'tool_results', [...calls].reverse().map(call =>
                toolResult(call, `Answer ${call.id}`))));
    }
    items.push(toolEvent(sequence, 'run_finished', { status: 'completed', exchanges }));
    return items;
}

/**
 * Count the previous hot-path array traversals, without production instrumentation
 * or timing assertions. Only dense generated call/assistant arrays are counted;
 * every native operation retains its return value and callback receiver. The
 * fixture is synchronous and restores all methods even if folding throws.
 */
export function withToolScanCounts(operation) {
    const originals = new Map();
    const counts = { callScanVisits: 0, claimedKeyCopies: 0, membershipVisits: 0 };
    const callArray = array => array[0] && Object.hasOwn(array[0], 'key')
        && Object.hasOwn(array[0], 'status') && Object.hasOwn(array[0], 'result');
    for (const name of ['some', 'find', 'filter']) {
        const original = Array.prototype[name];
        originals.set(name, original);
        Array.prototype[name] = function (predicate, receiver) {
            if (!callArray(this)) return original.call(this, predicate, receiver);
            return original.call(this, function (...args) {
                counts.callScanVisits += 1;
                return predicate.apply(this, args);
            }, receiver);
        };
    }
    for (const name of ['indexOf', 'includes', 'flatMap']) {
        const original = Array.prototype[name];
        originals.set(name, original);
        Array.prototype[name] = function (...args) {
            const result = original.apply(this, args);
            if (name === 'indexOf' && callArray(this) && args.length === 1) {
                counts.callScanVisits += result < 0 ? this.length : result + 1;
            } else if (name === 'includes' && typeof this[0] === 'string'
                && this[0].startsWith('call-') && args.length === 1) {
                const position = originals.get('indexOf').call(this, args[0]);
                counts.membershipVisits += position < 0 ? this.length : position + 1;
            } else if (name === 'flatMap' && this[0] && Object.hasOwn(this[0], 'callIds')) {
                counts.claimedKeyCopies += result.length;
            }
            return result;
        };
    }
    try {
        return { value: operation(), counts };
    } finally {
        for (const [name, original] of originals) Array.prototype[name] = original;
    }
}
