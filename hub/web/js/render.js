/**
 * @file pure renderers: one worker envelope (or one hub record) becomes DOM.
 *
 * No hub state and no network here — every function depends only on its
 * arguments. All untrusted text (model output, tool output, log lines, ids,
 * JSON) is inserted with `textContent`; nothing in this module uses innerHTML.
 */

/** Rendering hints mirrored from hub/src/protocol/events.js. */
export const EVENT_META = {
    ready: { tone: 'info', note: 'worker startup finished' },
    status: { tone: 'info', note: 'state snapshot' },
    options: { tone: 'info', note: 'available choices and selections' },
    input_admitted: { tone: 'info', note: 'host admitted an input' },
    input_rejected: { tone: 'warn', note: 'dequeued input failed validation' },
    run_started: { tone: 'info', note: 'loop admitted the invocation' },
    input_committed: { tone: 'info', note: 'user input integrated in memory' },
    model_response: { tone: 'assistant', note: 'complete model response' },
    tool_calls: { tone: 'tool', note: 'calls proposed for a batch' },
    tool_results: { tone: 'tool', note: 'complete returned batch' },
    persisted: { tone: 'muted', note: 'JSON snapshot written' },
    export_error: { tone: 'warn', note: 'Markdown export failed' },
    error: { tone: 'error', note: 'control or storage diagnostic' },
    run_finished: { tone: 'summary', note: 'invocation settled' },
};

/** Build one element. Children are appended (`append` treats strings as text). */export function el(tag, options = {}, children = []) {
    const node = document.createElement(tag);
    if (options.class) node.className = options.class;
    if (options.text !== undefined && options.text !== null) node.textContent = String(options.text);
    if (options.title) node.title = String(options.title);
    if (options.hidden) node.hidden = true;
    if (options.attrs) {
        for (const [key, value] of Object.entries(options.attrs)) {
            if (value === null || value === undefined || value === false) continue;
            node.setAttribute(key, String(value));
        }
    }
    if (options.dataset) {
        for (const [key, value] of Object.entries(options.dataset)) {
            if (value !== null && value !== undefined) node.dataset[key] = String(value);
        }
    }
    if (options.on) {
        for (const [event, handler] of Object.entries(options.on)) {
            node.addEventListener(event, handler);
        }
    }
    for (const child of Array.isArray(children) ? children : [children]) {
        if (child === null || child === undefined || child === false) continue;
        node.append(child);
    }
    return node;
}

/** Remove every child of a node. */
export function clearNode(node) {
    while (node.firstChild) node.removeChild(node.firstChild);
    return node;
}

/** Treat any non-object as an empty object. */
function obj(value) {
    return value && typeof value === 'object' && !Array.isArray(value) ? value : {};
}

/** Public alias of `obj` for callers that need the same coercion. */
export const objSafe = obj;

/** Readable text for an unknown JSON value. */
export function textOf(value) {
    if (value === null || value === undefined) return '';
    if (typeof value === 'string') return value;
    if (typeof value === 'number' || typeof value === 'boolean') return String(value);
    try {
        return JSON.stringify(value, null, 2);
    } catch {
        return String(value);
    }
}

/** Pretty JSON, tolerating cyclic or otherwise unserializable values. */
export function pretty(value) {
    if (typeof value === 'string') return value;
    try {
        return JSON.stringify(value, null, 2) ?? String(value);
    } catch {
        return String(value);
    }
}

/** Local clock time (HH:MM:SS) for an ISO timestamp. */
export function formatClock(iso) {
    if (typeof iso !== 'string' || iso.length === 0) return '—';
    const date = new Date(iso);
    if (Number.isNaN(date.getTime())) return iso;
    return date.toLocaleTimeString();
}

/** Local date+time for an ISO timestamp. */
export function formatStamp(iso) {
    if (typeof iso !== 'string' || iso.length === 0) return '—';
    const date = new Date(iso);
    if (Number.isNaN(date.getTime())) return iso;
    return date.toLocaleString();
}

/** First `size` characters of an identifier, never rendering an empty id. */
export function shortId(id, size = 8) {
    if (typeof id !== 'string' || id.length === 0) return '—';
    return id.length <= size ? id : id.slice(0, size);
}

/** Approximate decoded byte length of base64 text. */
export function base64Bytes(raw) {
    if (typeof raw !== 'string') return 0;
    const padding = raw.endsWith('==') ? 2 : raw.endsWith('=') ? 1 : 0;
    return Math.max(0, Math.floor((raw.length * 3) / 4) - padding);
}

/** Milliseconds remaining until an ISO deadline (negative when passed). */
export function millisUntil(iso, now = Date.now()) {
    const date = new Date(iso);
    if (Number.isNaN(date.getTime())) return null;
    return date.getTime() - now;
}

/** Countdown text for a deadline, or '' when there is no usable deadline. */
export function countdownText(iso, now = Date.now()) {
    const remaining = millisUntil(iso, now);
    if (remaining === null) return 'no deadline reported';
    if (remaining <= 0) return 'deadline passed';
    const totalSeconds = Math.ceil(remaining / 1000);
    const minutes = Math.floor(totalSeconds / 60);
    const seconds = totalSeconds % 60;
    return `${minutes}:${String(seconds).padStart(2, '0')} left`;
}

/** Small bordered label. */
export function chip(label, tone = 'muted', extra = {}) {
    return el('span', { class: `chip chip-${tone}`, text: label, title: extra.title ?? label });
}

/** Preformatted JSON block. */
export function jsonBlock(value, className = 'json') {
    return el('pre', { class: className, text: pretty(value) });
}

/** Definition list from [label, value] pairs; preserves insertion order. */
export function kv(pairs) {
    const list = el('dl', { class: 'kv' });
    for (const [key, value] of pairs) {
        if (value === undefined || value === null || value === '') continue;
        list.append(el('dt', { text: key }));
        if (value instanceof Node) list.append(el('dd', {}, [value]));
        else list.append(el('dd', { text: String(value) }));
    }
    return list;
}

/** Standard empty state. */
export function emptyState(title, detail = '') {
    return el('div', { class: 'empty' }, [
        el('strong', { text: title }),
        detail ? el('span', { text: detail }) : null,
    ]);
}

/** Footer shared by every card: sequence, hub_sequence, local time, issues. */
export function envelopeFooter(envelope) {
    const bits = [
        `seq ${envelope?.sequence ?? '—'}`,
        `hub ${envelope?.hub_sequence ?? '—'}`,
        formatClock(envelope?.received_at),
    ];
    const foot = el('div', { class: 'card-foot' }, bits.map((value) => el('span', { text: value })));
    const issues = Array.isArray(envelope?.issues) ? envelope.issues : [];
    if (issues.length > 0) {
        foot.append(el('span', {
            class: 'marker',
            text: `issues: ${issues.length}`,
            title: issues.join('\n'),
        }));
    }
    return foot;
}

/** One card (optionally collapsible) with a head, a body and the footer. */
function card(tone, head, body, envelope, options = {}) {
    const headNode = el('div', { class: 'card-head' }, head);
    const bodyNode = el('div', { class: 'card-body' }, body);
    const foot = envelopeFooter(envelope);
    if (options.collapsed) {
        return el('details', { class: `card tone-${tone}` }, [
            el('summary', {}, [headNode]),
            bodyNode,
            foot,
        ]);
    }
    return el('article', { class: `card tone-${tone}` }, [headNode, bodyNode, foot]);
}

/** Compact chip line used by run-boundary and persistence events. */
function chipRow(chips, envelope, note = '') {
    const row = el('div', { class: 'chip-row' }, chips);
    if (note) row.append(el('span', { class: 'card-note', text: note }));
    row.append(el('span', {
        class: 'chip-meta',
        text: `seq ${envelope?.sequence ?? '—'} · hub ${envelope?.hub_sequence ?? '—'}`
            + ` · ${formatClock(envelope?.received_at)}`,
    }));
    return row;
}

/** Raw-JSON escape hatch attached to a card body. */
function rawToggle(label, value) {
    return el('details', { class: 'line' }, [
        el('summary', {}, [el('span', { text: label })]),
        el('div', { class: 'line-body' }, [jsonBlock(value)]),
    ]);
}

// --------------------------------------------------------------- content --

/**
 * One Content value (`{type, raw, extras}`): text, external reference or
 * binary. Remote references are never auto-loaded; a binary blob is never
 * decoded without an explicit click.
 */
export function renderContentValue(part) {
    if (typeof part === 'string') return el('div', { class: 'content-text', text: part });
    const value = obj(part);
    const type = typeof value.type === 'string' ? value.type : 'unknown';
    const raw = typeof value.raw === 'string' ? value.raw : textOf(value.raw);
    if (type === 'text') {
        const node = el('div', { class: 'content-text' }, [el('span', { text: raw })]);
        if (value.extras !== undefined) node.append(rawToggle('extras', value.extras));
        return node;
    }
    if (type === 'external_ref') {
        const container = el('div', { class: 'content-ref' });
        const isHttp = /^https?:\/\//i.test(raw);
        const label = el('span', { class: 'card-note', text: 'external reference (not fetched):' });
        if (isHttp) {
            container.append(label, el('a', {
                text: raw,
                attrs: { href: raw, target: '_blank', rel: 'noreferrer noopener' },
            }));
        } else {
            container.append(label);
        }
        container.append(el('span', { class: 'ref-raw', text: raw }));
        if (value.extras !== undefined) container.append(rawToggle('extras', value.extras));
        return container;
    }
    if (type === 'binary') {
        const container = el('div', { class: 'content-binary' });
        container.append(el('span', {
            class: 'bin-note',
            text: `binary content: base64, ≈${base64Bytes(raw)} bytes decoded`,
        }));
        const pre = el('pre', { class: 'json', text: raw, hidden: true });
        const toggle = el('button', {
            class: 'btn btn-sm',
            text: 'show base64',
            attrs: { type: 'button' },
            on: {
                click: () => {
                    pre.hidden = !pre.hidden;
                    toggle.textContent = pre.hidden ? 'show base64' : 'hide base64';
                },
            },
        });
        container.append(el('div', {}, [toggle]), pre);
        if (value.extras !== undefined) container.append(rawToggle('extras', value.extras));
        return container;
    }
    return el('div', {}, [
        el('span', { class: 'card-note', text: `content type "${type}"` }),
        rawToggle('content raw', value),
    ]);
}

/** Ordered content parts. */
export function renderContentParts(parts) {
    const list = Array.isArray(parts) ? parts : [];
    if (list.length === 0) return [el('div', { class: 'card-note', text: '(no content parts)' })];
    return list.map((part) => renderContentValue(part));
}

/** Token cost footer. `cache_hit` is already part of `prompt`. */
export function costLine(cost) {
    const value = obj(cost);
    const numbers = ['prompt', 'generated', 'cache_hit']
        .filter((key) => typeof value[key] === 'number');
    if (numbers.length === 0) return el('div', { class: 'cost' }, [el('span', { text: 'cost: reported, no token counts' })]);
    const total = (typeof value.prompt === 'number' ? value.prompt : 0)
        + (typeof value.generated === 'number' ? value.generated : 0);
    const bits = numbers.map((key) => `${key} ${value[key]}`);
    bits.push(`total ${total} (prompt + generated)`);
    if (typeof value.cache_hit === 'number') bits.push('cache_hit is included in prompt');
    return el('div', { class: 'cost' }, bits.map((text) => el('span', { text })));
}

// ------------------------------------------------------------ call/result --

/** One Call object: name, scheduling type, security, settled arguments. */
export function renderCall(call) {
    const value = obj(call);
    const head = el('div', { class: 'call-head' }, [
        el('span', { class: 'call-name', text: value.name ?? '(unnamed call)' }),
        value.type ? el('span', { class: 'mini-badge', text: value.type }) : null,
        value.security ? el('span', { class: 'mini-badge', text: value.security }) : null,
        value.id ? el('span', { class: 'card-note mono', text: `id ${value.id}` }) : null,
    ]);
    const container = el('div', { class: 'call' }, [head]);
    container.append(jsonBlock(value.arguments ?? {}, 'json'));
    if (value.extras !== undefined) container.append(rawToggle('extras', value.extras));
    return container;
}

/** Recognized result annotations; unknown extras stay visible below. */
function resultAnnotations(extras) {
    const nodes = [];
    if (!extras || typeof extras !== 'object') return nodes;
    if (extras.error && typeof extras.error === 'object') {
        const stage = typeof extras.error.stage === 'string' ? extras.error.stage : 'unknown';
        const message = typeof extras.error.message === 'string' ? extras.error.message : '';
        nodes.push(el('div', {
            class: 'annot annot-error',
            text: `error at ${stage}: ${message || '(no message)'}`,
            title: 'a tool error does not imply that no side effect occurred',
        }));
    }
    if (extras.loop_skipped === true) {
        nodes.push(el('div', {
            class: 'annot annot-skipped',
            text: 'loop_skipped: the loop did not dispatch this call — a non-execution result, not tool output',
        }));
    }
    if (extras.cause_query && typeof extras.cause_query === 'object') {
        nodes.push(el('details', { class: 'annot annot-cause' }, [
            el('summary', {}, [el('span', { text: 'cause_query (a different call involved in this result)' })]),
            renderCall(extras.cause_query),
        ]));
    }
    return nodes;
}

/**
 * One `tool_results` entry.
 *
 * The documented shape is a Result object (`{query, output, extras}`). Core
 * currently projects results as tool messages instead (`{content, invoke_return,
 * role, type}`), so both are accepted: provenance comes from `invoke_return`
 * when the entry has no top-level `query`/`output`.
 */
export function renderResult(result) {
    const value = obj(result);
    const provenance = obj(value.invoke_return);
    const query = obj(value.query ?? provenance.query);
    const output = value.output ?? provenance.output ?? null;
    const extras = value.extras ?? provenance.extras;
    const head = el('div', { class: 'result-head' }, [
        el('span', { class: 'result-name', text: query.name ?? value.type ?? '(unnamed result)' }),
        query.id ? el('span', { class: 'card-note mono', text: `id ${query.id}` }) : null,
        query.type ? el('span', { class: 'mini-badge', text: query.type }) : null,
        query.security ? el('span', { class: 'mini-badge', text: query.security }) : null,
        value.type === 'invoke_return' && value.role
            ? el('span', { class: 'card-note', text: `projected as ${value.role}/${value.type} message` })
            : null,
    ]);
    const container = el('div', { class: 'result' }, [head]);
    for (const node of resultAnnotations(obj(extras))) container.append(node);
    if (output) {
        container.append(renderContentValue(output));
    } else if (Array.isArray(value.content)) {
        for (const part of renderContentParts(value.content)) container.append(part);
    } else {
        container.append(el('div', {
            class: 'card-note',
            text: 'no output content in this result entry',
        }));
    }
    if (extras !== undefined) container.append(rawToggle('extras (raw)', extras));
    if (value.invoke_return !== undefined) {
        container.append(rawToggle('invoke_return (raw)', value.invoke_return));
    }
    if (Array.isArray(value.content) && output) {
        container.append(rawToggle('message content (raw)', value.content));
    }
    if (Object.keys(query).length > 0) container.append(rawToggle('query (raw)', query));
    return container;
}

// ---------------------------------------------------------- event renderers --

/** `ready` / `status`: a muted collapsible line with the status object. */
function statusLine(envelope, name) {
    const data = obj(envelope.data);
    const loop = obj(data.loop);
    const storageFailed = data.storage_failed === true;
    const summary = [name];
    summary.push(data.active === true ? 'active' : 'stopped');
    if (typeof loop.status === 'string') summary.push(`loop ${loop.status}`);
    if (typeof loop.phase === 'string') summary.push(`phase ${loop.phase}`);
    if (typeof loop.completed_exchanges === 'number') {
        summary.push(`exchanges ${loop.completed_exchanges}`);
    }
    if (storageFailed) summary.push('storage_failed');
    const summaryNode = el('summary', {}, [
        el('span', { class: 'mono', text: summary.join(' · ') }),
        storageFailed ? el('span', { class: 'badge badge-error', text: 'storage failed' }) : null,
    ]);
    const body = el('div', { class: 'line-body' }, [statusBody(data), envelopeFooter(envelope)]);
    return el('details', { class: `line${storageFailed ? ' tone-error' : ''}` }, [summaryNode, body]);
}

/** Readable status object: flags, loop progress, pending results. */
export function statusBody(data) {
    const value = obj(data);
    const nodes = [];
    if (value.storage_failed === true) {
        nodes.push(el('div', {
            class: 'alert alert-error',
            text: 'storage_failed: a required JSON persistence operation failed; further saves are suppressed',
        }));
    }
    nodes.push(kv([
        ['active', value.active === true ? 'true' : 'false'],
        ['stopping', value.stopping === true ? 'true' : 'false'],
        ['storage_failed', value.storage_failed === true ? 'true' : 'false'],
        ['rejected_payloads', typeof value.rejected_payloads === 'number' ? value.rejected_payloads : ''],
    ]));
    if (value.loop && typeof value.loop === 'object') nodes.push(loopBlock(value.loop));
    else nodes.push(el('div', { class: 'card-note', text: 'no loop progress in this snapshot' }));
    return el('div', { class: 'kv-section' }, nodes);
}

/** Loop-progress fields, rendered readably. */
export function loopBlock(loop) {
    const value = obj(loop);
    const rows = [
        ['loop.status', value.status],
        ['loop.phase', value.phase],
        ['completed_exchanges', value.completed_exchanges],
        ['committed_response_sequence', value.committed_response_sequence],
        ['error', value.error],
    ];
    const sections = [
        el('div', { class: 'kv-title', text: 'loop progress' }),
        kv(rows),
    ];
    if (Array.isArray(value.pending_results) && value.pending_results.length > 0) {
        sections.push(el('div', {
            class: 'alert alert-warn',
            text: `${value.pending_results.length} pending result(s) awaiting projection`,
        }));
        sections.push(jsonBlock(value.pending_results));
    }
    if (value.phase === 'tools' || value.phase === 'blocked') {
        sections.push(el('div', {
            class: 'alert alert-warn',
            text: `recovery boundary: phase "${value.phase}" — the worker is resuming from a persisted checkpoint`,
        }));
    }
    return el('div', { class: 'kv-section' }, sections);
}

function persistedChip(envelope) {
    const data = obj(envelope.data);
    const boundary = data.boundary ?? 'unknown boundary';
    return chipRow([chip(`persisted · ${boundary}${data.format ? ` · ${data.format}` : ''}`, 'muted')], envelope);
}

function runFinishedChip(envelope) {
    const data = obj(envelope.data);
    const status = typeof data.status === 'string' ? data.status : 'unknown';
    const tone = status === 'failed' ? 'error'
        : status === 'cancelled' || status === 'exchange_limit' ? 'warn' : 'ok';
    const durable = data.durable === true ? 'durable' : 'not durable';
    const exchanges = typeof data.exchanges === 'number' ? ` · exchanges ${data.exchanges}` : '';
    const chips = [chip(`run_finished · ${status}${exchanges} · ${durable}`, tone)];
    if (data.error) chips.push(chip(`error: ${data.error}`, 'error'));
    return chipRow(chips, envelope);
}

function inputRejectedCard(envelope) {
    const data = obj(envelope.data);
    const body = [
        el('div', { class: 'alert alert-warn', text: data.message ?? '(no message)' }),
        kv([
            ['request_id', data.request_id === undefined ? '' : textOf(data.request_id)],
            ['run_id', envelope.run_id ?? ''],
        ]),
    ];
    if (body[1].children.length > 0) {
        body[1].prepend(el('div', { class: 'kv-title', text: 'echoed correlation' }));
    }
    return card('warn', [
        el('span', { class: 'card-title', text: 'input_rejected' }),
        el('span', { class: 'card-note', text: EVENT_META.input_rejected.note }),
    ], body, envelope);
}

function modelResponseCard(envelope) {
    const data = obj(envelope.data);
    const body = renderContentParts(data.content);
    if (data.reasoning) {
        body.push(el('details', { class: 'line' }, [
            el('summary', {}, [el('span', { text: 'reasoning' })]),
            el('div', { class: 'line-body' }, renderContentParts([data.reasoning])),
        ]));
    }
    if (data.action_status) {
        body.push(el('details', { class: 'line' }, [
            el('summary', {}, [el('span', { text: 'action_status' })]),
            el('div', { class: 'line-body' }, renderContentParts([data.action_status])),
        ]));
    }
    const invokes = Array.isArray(data.invokes) ? data.invokes : [];
    if (invokes.length > 0) {
        body.push(el('details', { class: 'line' }, [
            el('summary', {}, [el('span', { text: `invokes (${invokes.length} proposed call(s))` })]),
            el('div', { class: 'line-body' }, invokes.map(renderCall)),
        ]));
    }
    if (data.cost !== undefined) body.push(costLine(data.cost));
    body.push(rawToggle('raw message object', data));
    const head = [
        el('span', { class: 'card-title', text: 'model_response' }),
        el('span', {
            class: 'card-note',
            text: [data.role, data.type].filter((value) => typeof value === 'string' && value).join(' · '),
        }),
    ];
    return card('assistant', head, body, envelope);
}

function toolCallsCard(envelope) {
    const data = Array.isArray(envelope.data) ? envelope.data
        : Array.isArray(obj(envelope.data).calls) ? obj(envelope.data).calls : [];
    const body = [
        el('div', {
            class: 'alert alert-info',
            text: 'proposals from the model, before dispatch and security evaluation —'
                + ' not proof of execution and not an authorization',
        }),
        ...data.map(renderCall),
        rawToggle('raw', envelope.data),
    ];
    return card('tool', [
        el('span', { class: 'card-title', text: 'tool_calls' }),
        el('span', { class: 'card-note', text: `${data.length} proposed call(s)` }),
    ], body, envelope);
}

function toolResultsCard(envelope) {
    const data = Array.isArray(envelope.data) ? envelope.data : [];
    const body = [
        el('div', {
            class: 'alert alert-info',
            text: 'results in call order, not completion order; there is no success boolean in the envelope',
        }),
        ...data.map(renderResult),
        rawToggle('raw', envelope.data),
    ];
    return card('tool', [
        el('span', { class: 'card-title', text: 'tool_results' }),
        el('span', { class: 'card-note', text: `${data.length} result(s)` }),
    ], body, envelope);
}

function errorCard(envelope, exported) {
    const data = obj(envelope.data);
    const body = [el('div', { class: `alert ${exported ? 'alert-warn' : 'alert-error'}`, text: data.message ?? '(no message)' })];
    if (!exported && Object.hasOwn(data, 'durable')) {
        body.push(el('span', {
            class: 'marker',
            text: `durable: ${data.durable === true}`,
        }));
        if (data.durable === false) {
            body.push(el('div', {
                class: 'card-note',
                text: 'durable:false also occurs when persistence or final saving is disabled;'
                    + ' it is not by itself an IO error',
            }));
        }
    }
    body.push(rawToggle('raw', data));
    return card(exported ? 'warn' : 'error', [
        el('span', { class: 'card-title', text: exported ? 'export_error' : 'error' }),
        el('span', { class: 'card-note', text: `${EVENT_META[exported ? 'export_error' : 'error'].note}`
            + ' — not a universal fatal-error notification' }),
    ], body, envelope);
}

function optionsCard(envelope) {
    const summary = el('summary', {}, [
        el('span', { class: 'card-title', text: 'options' }),
        el('span', { class: 'card-note', text: EVENT_META.options.note }),
    ]);
    return el('details', { class: 'card tone-info' }, [
        summary,
        el('div', { class: 'card-body' }, [jsonBlock(envelope.data)]),
        envelopeFooter(envelope),
    ]);
}

function unknownCard(envelope, name) {
    return el('details', { class: 'card tone-muted' }, [
        el('summary', {}, [
            el('span', { class: 'card-title', text: name }),
            el('span', { class: 'badge badge-warn', text: 'unknown event' }),
        ]),
        el('div', { class: 'card-body' }, [
            el('div', {
                class: 'card-note',
                text: 'this panel has no renderer for that event name; the payload is shown as received',
            }),
            jsonBlock(envelope.data),
            rawToggle('raw envelope', envelope.raw ?? envelope.data),
        ]),
        envelopeFooter(envelope),
    ]);
}

/**
 * One transcript card for one worker envelope.
 *
 * @param {object} envelope hub envelope (`event`, `data`, `hub_sequence`, …).
 * @returns {HTMLElement}
 */
export function renderEnvelope(envelope) {
    const name = typeof envelope?.event === 'string' && envelope.event.length > 0
        ? envelope.event : '(unnamed event)';
    switch (name) {
        case 'ready':
        case 'status':
            return statusLine(envelope, name);
        case 'options':
            return optionsCard(envelope);
        case 'input_admitted':
            return chipRow([chip(`input_admitted · request ${shortId(envelope.request_id)}`, 'info')], envelope);
        case 'input_committed':
            return chipRow([chip('input_committed · input integrated in memory', 'info')], envelope);
        case 'run_started':
            return chipRow([chip(`run_started · run ${shortId(envelope.run_id)}`, 'info')], envelope);
        case 'persisted':
            return persistedChip(envelope);
        case 'run_finished':
            return runFinishedChip(envelope);
        case 'input_rejected':
            return inputRejectedCard(envelope);
        case 'model_response':
            return modelResponseCard(envelope);
        case 'tool_calls':
            return toolCallsCard(envelope);
        case 'tool_results':
            return toolResultsCard(envelope);
        case 'error':
            return errorCard(envelope, false);
        case 'export_error':
            return errorCard(envelope, true);
        default:
            return unknownCard(envelope, name);
    }
}

/** One collapsible block for the raw-events tab. */
export function renderRawEnvelope(envelope) {
    const issues = Array.isArray(envelope?.issues) ? envelope.issues : [];
    const body = el('div', { class: 'line-body' }, [
        kv([
            ['event', envelope?.event ?? ''],
            ['hub_sequence', envelope?.hub_sequence ?? ''],
            ['sequence', envelope?.sequence ?? ''],
            ['received_at', envelope?.received_at ? formatStamp(envelope.received_at) : ''],
            ['worker_id', envelope?.worker_id ?? ''],
            ['run_id', envelope?.run_id ?? ''],
            ['request_id', envelope?.request_id ?? ''],
            ['known', envelope?.known === true ? 'true' : 'false'],
            ['session_id', envelope?.session_id ?? ''],
        ]),
        issues.length > 0
            ? el('div', { class: 'alert alert-warn', text: `validation issues: ${issues.join('; ')}` })
            : el('div', { class: 'card-note', text: 'no validation issues' }),
        jsonBlock(envelope?.raw ?? envelope?.data ?? {}),
    ]);
    return el('details', { class: 'card tone-muted' }, [
        el('summary', {}, [
            el('span', { class: 'card-title mono', text: `#${envelope?.hub_sequence ?? '—'} ${envelope?.event ?? ''}` }),
            el('span', { class: 'card-note', text: formatClock(envelope?.received_at) }),
        ]),
        body,
    ]);
}

/** Chip for one tracked request outcome from the hub. */
export function renderRequestChip(request) {
    const entry = obj(request);
    const state = typeof entry.state === 'string' ? entry.state : 'unknown';
    const operation = typeof entry.operation === 'string' ? entry.operation : 'input';
    const id = shortId(entry.request_id);
    let tone = 'info';
    let label = `${operation} · ${id} · ${state}`;
    if (state === 'unknown') {
        tone = 'warn';
        label = `${operation} · ${id} · outcome unknown — do not assume it ran`;
    } else if (state === 'rejected') {
        tone = 'error';
    } else if (state === 'admitted') {
        tone = 'ok';
    }
    const node = chip(label, tone, {
        title: [`state: ${state}`, entry.sent_at ? `sent: ${formatStamp(entry.sent_at)}` : '',
            entry.settled_at ? `settled: ${formatStamp(entry.settled_at)}` : '',
            entry.detail ? `detail: ${entry.detail}` : ''].filter(Boolean).join('\n'),
    });
    node.classList.add('chip-request');
    if (state === 'unknown') node.classList.add('chip-request-unknown');
    node.dataset.requestId = String(entry.request_id ?? '');
    const row = el('div', { class: 'chip-row' }, [node]);
    if (entry.detail && state !== 'unknown') {
        row.append(el('span', { class: 'card-note', text: String(entry.detail) }));
    }
    return row;
}

// ------------------------------------------------------------- inspector --

/** Options snapshot: available descriptors as selects, plus current values. */
export function renderOptionsPanel(data) {
    const root = el('div', { class: 'ipane' });
    if (!data || typeof data !== 'object') {
        root.append(emptyState('No options yet', 'Send the Options signal to ask the worker for its choices.'));
        return root;
    }
    const categories = Object.keys(data);
    if (categories.length === 0) {
        root.append(emptyState('Empty options snapshot', 'The worker reported no categories.'));
        return root;
    }
    for (const category of categories) {
        const value = obj(data[category]);
        const available = Array.isArray(value.available) ? value.available : [];
        const current = obj(value.current);
        const section = el('section', { class: 'opt-cat' }, [
            el('h3', { text: category }),
        ]);
        if (available.length === 0) {
            section.append(el('div', {
                class: 'opt-note',
                text: category === 'tools'
                    ? 'tool configuration is reserved; empty metadata does not mean tools are disabled'
                    : 'the worker advertises no choices for this category',
            }));
        }
        for (const descriptor of available) {
            const item = obj(descriptor);
            if (typeof item.name !== 'string') continue;
            const options = Array.isArray(item.options) ? item.options : [];
            const select = el('select', {
                attrs: { 'aria-label': `${category}.${item.name}` },
                dataset: { category, option: item.name },
            });
            const currentValue = current[item.name];
            const knownValues = options.map((option) => String(option));
            if (currentValue !== undefined && !knownValues.includes(String(currentValue))) {
                select.append(el('option', { text: `${textOf(currentValue)} (current, not advertised)`, attrs: { value: String(currentValue) } }));
            }
            for (const option of options) {
                select.append(el('option', { text: String(option), attrs: { value: String(option) } }));
            }
            if (currentValue !== undefined) select.value = String(currentValue);
            section.append(el('div', { class: 'opt-row' }, [
                el('span', { class: 'opt-name', text: item.name }),
                select,
            ]));
        }
        const currentKeys = Object.keys(current);
        if (currentKeys.length > 0) {
            section.append(kv(currentKeys.map((key) => [`current.${key}`, textOf(current[key])])));
        } else {
            section.append(el('div', { class: 'opt-note', text: 'no current values reported' }));
        }
        root.append(section);
    }
    root.append(el('div', {
        class: 'opt-note',
        text: 'Selections here are not signals: they are attached to the next payload as'
            + ' data.options and take effect at the next run boundary.',
    }));
    return root;
}

/** Read the option selections currently shown in an options panel. */
export function readOptionSelections(root) {
    const result = {};
    for (const select of root.querySelectorAll('select[data-category][data-option]')) {
        const category = select.dataset.category;
        const name = select.dataset.option;
        if (!result[category]) result[category] = {};
        result[category][name] = select.value;
    }
    return result;
}

/** Process record fields. */
export function renderProcessPanel(process) {
    const root = el('div', { class: 'ipane' });
    if (!process) {
        root.append(emptyState('No process record', 'Start the worker to let the hub supervise a process for this session.'));
        return root;
    }
    const value = obj(process);
    root.append(kv([
        ['state', value.state],
        ['pid', value.pid],
        ['started_at', value.started_at ? formatStamp(value.started_at) : ''],
        ['exited_at', value.exited_at ? formatStamp(value.exited_at) : ''],
        ['stop_requested', value.stop_requested === true ? 'true' : 'false'],
        ['process_group_killed', value.process_group_killed === true ? 'true' : 'false'],
        ['log_lines', value.log_lines],
        ['log_dropped', value.log_dropped],
    ]));
    const exitProblem = (typeof value.exit_code === 'number' && value.exit_code !== 0)
        || (typeof value.signal === 'string' && value.signal.length > 0);
    root.append(kv([
        ['exit_code', typeof value.exit_code === 'number' ? value.exit_code : '—'],
        ['signal', value.signal ?? '—'],
    ]));
    if (exitProblem) {
        root.append(el('div', {
            class: 'alert alert-error',
            text: `the process did not exit cleanly (code ${value.exit_code ?? '—'}, signal ${value.signal ?? 'none'})`,
        }));
    }
    if (value.error) root.append(el('div', { class: 'alert alert-error', text: String(value.error) }));
    root.append(kv([
        ['cwd', value.cwd],
        ['command', value.command],
    ]));
    if (Array.isArray(value.args) && value.args.length > 0) {
        root.append(el('div', { class: 'kv-title', text: 'args' }));
        root.append(jsonBlock(value.args));
    }
    if (value.log_path) {
        root.append(el('div', { class: 'kv-title', text: 'captured output file' }));
        root.append(el('div', { class: 'mono', text: String(value.log_path) }));
    }
    return root;
}

/** Worker log tail plus dropped-line notice. */
export function renderLogsPanel(logs) {
    const value = obj(logs);
    const lines = Array.isArray(value.lines) ? value.lines : [];
    const root = el('div', { class: 'ipane' });
    if (value.logPath) root.append(el('div', { class: 'opt-note mono', text: `file: ${value.logPath}` }));
    if (typeof value.dropped === 'number' && value.dropped > 0) {
        root.append(el('div', {
            class: 'alert alert-warn',
            text: `${value.dropped} line(s) were dropped from the hub's bounded capture; the on-disk log is authoritative`,
        }));
    }
    if (lines.length === 0) {
        root.append(emptyState('No captured output', 'The hub captures worker stdout/stderr after the worker starts.'));
        return root;
    }
    root.append(el('pre', { class: 'log-tail', text: lines.join('\n') }));
    root.append(el('div', { class: 'opt-note', text: `${lines.length} line(s) shown (client cap applied)` }));
    return root;
}

/**
 * Snapshot viewer: read-only, worker-owned.
 *
 * @param {object|null} snapshot response of GET /api/sessions/:id/snapshot.
 */
export function renderSnapshotPanel(snapshot) {
    const root = el('div', { class: 'ipane' });
    root.append(el('div', {
        class: 'alert alert-info',
        text: 'read-only copy of the worker-owned persistence directory.'
            + ' The hub never writes here; the worker is the only writer.',
    }));
    if (!snapshot) {
        root.append(emptyState('Snapshot not loaded', 'Use "Load snapshot" to read state.json and readable.md from disk.'));
        return root;
    }
    if (snapshot.state_error) {
        root.append(el('div', { class: 'alert alert-error', text: `state.json could not be parsed: ${snapshot.state_error}` }));
    }
    if (snapshot.files?.state) root.append(el('div', { class: 'opt-note mono', text: `file: ${snapshot.files.state}` }));
    if (!snapshot.state) {
        root.append(emptyState('No state.json yet', 'The worker writes it at persistence boundaries; nothing has been persisted so far.'));
    } else {
        const state = obj(snapshot.state);
        const keys = Object.keys(state);
        root.append(el('div', { class: 'kv-title', text: `state.json · ${keys.length} top-level key(s)` }));
        root.append(kv(keys.slice(0, 40).map((key) => {
            const value = state[key];
            const kind = Array.isArray(value) ? `array(${value.length})` : typeof value === 'object' && value !== null ? 'object' : textOf(value);
            return [key, kind];
        })));
        root.append(el('details', { class: 'line' }, [
            el('summary', {}, [el('span', { text: 'state.json (full)' })]),
            el('div', { class: 'line-body' }, [jsonBlock(state)]),
        ]));
    }
    if (snapshot.files?.readable) root.append(el('div', { class: 'opt-note mono', text: `file: ${snapshot.files.readable}` }));
    if (typeof snapshot.readable === 'string') {
        root.append(el('div', { class: 'kv-title', text: 'readable.md' }));
        root.append(el('pre', { class: 'block', text: snapshot.readable }));
    } else {
        root.append(el('div', {
            class: 'opt-note',
            text: 'readable.md is absent — Markdown export is optional and can fail without affecting state.json',
        }));
    }
    return root;
}

// ---------------------------------------------------------- confirmations --

/**
 * One confirmation prompt as a modal dialog.
 *
 * @param {object} prompt prompt description from the hub.
 * @param {object} handlers
 * @param {(decision: 'approved'|'denied', reason: string) => void} handlers.onDecide
 * @param {() => void} handlers.onMinimize
 */
export function renderConfirmationModal(prompt, { onDecide, onMinimize } = {}) {
    const call = obj(prompt.call);
    const awaitingIdentity = prompt.state === 'awaiting-identity';
    const reasonInput = el('input', {
        attrs: {
            type: 'text',
            placeholder: 'reason (optional)',
            'aria-label': 'Decision reason',
            spellcheck: 'false',
        },
    });
    const errorLine = el('p', { class: 'modal-error' });
    const approve = el('button', {
        class: 'btn btn-primary',
        text: 'Approve',
        attrs: { type: 'button' },
        on: {
            click: () => {
                if (!reasonInput.value.trim()) onDecide?.('approved', 'operator decision');
                else onDecide?.('approved', reasonInput.value.trim());
                approve.disabled = true;
                deny.disabled = true;
            },
        },
    });
    const deny = el('button', {
        class: 'btn btn-danger',
        text: 'Deny',
        attrs: { type: 'button' },
        on: {
            click: () => {
                onDecide?.('denied', reasonInput.value.trim() || 'operator decision');
                approve.disabled = true;
                deny.disabled = true;
            },
        },
    });
    if (awaitingIdentity) {
        approve.disabled = true;
        deny.disabled = true;
    }
    const countdown = el('span', {
        class: 'countdown',
        text: countdownText(prompt.deadline_at),
        dataset: { deadline: prompt.deadline_at ?? '' },
    });
    const body = [
        el('div', { class: 'call' }, [
            el('div', { class: 'call-head' }, [
                el('span', { class: 'call-name', text: call.name ?? '(unnamed call)' }),
                call.type ? el('span', { class: 'mini-badge', text: call.type }) : null,
                call.security ? el('span', { class: 'mini-badge', text: call.security }) : null,
                call.id ? el('span', { class: 'card-note mono', text: `id ${call.id}` }) : null,
            ]),
            jsonBlock(call.arguments ?? {}),
        ]),
        kv([
            ['session', prompt.session_id],
            ['run_id', prompt.run_id],
            ['worker_id', prompt.worker_id],
            ['confirmation_id', prompt.confirmation_id],
            ['state', prompt.state],
            ['verified', prompt.verified === true ? 'true' : 'false'],
            ['identity', prompt.identity_state],
            ['received', prompt.received_at ? formatStamp(prompt.received_at) : ''],
        ]),
    ];
    if (awaitingIdentity) {
        body.push(el('div', {
            class: 'alert alert-warn',
            text: 'awaiting-identity: the hub is waiting for the event connection to identify this worker.'
                + ' The decision buttons stay disabled until the prompt is verified.',
        }));
    }
    body.push(el('div', { class: 'opt-note' }, [
        el('span', { text: 'advisory deadline: ' }),
        countdown,
        el('span', {
            text: ' — the worker\'s own deadline started earlier, and disconnection retires the prompt.'
                + ' The hub cannot confirm that an approved call actually ran.',
        }),
    ]));
    body.push(reasonInput, errorLine);
    const modal = el('div', {
        class: 'modal',
        attrs: { role: 'dialog', 'aria-modal': 'true', 'aria-label': `Confirmation for ${call.name ?? 'call'}` },
    }, [
        el('div', { class: 'modal-head' }, [
            el('span', { class: 'modal-title', text: `approval required · ${call.name ?? 'call'}` }),
            el('button', {
                class: 'btn btn-ghost btn-sm',
                text: 'hide',
                attrs: { type: 'button', 'aria-label': 'Hide this confirmation' },
                on: { click: () => onMinimize?.() },
            }),
        ]),
        el('div', { class: 'modal-body' }, body),
        el('div', { class: 'modal-foot' }, [
            el('span', { class: 'opt-note', text: 'deciding sends one response on the confirmation socket' }),
            el('span', { class: 'spacer' }),
            deny,
            approve,
        ]),
    ]);
    return modal;
}

// ---------------------------------------------------------- new session --

/**
 * New-session form.
 *
 * @param {object} options
 * @param {object} options.hub hub metadata (`provider_profiles`).
 * @param {(payload: {session: string, spec: object}) => void} options.onSubmit
 * @returns {{element: HTMLElement, read: Function, setError: Function, setBusy: Function}}
 */
export function renderNewSessionForm({ hub, onSubmit } = {}) {
    const profiles = Array.isArray(hub?.provider_profiles) && hub.provider_profiles.length > 0
        ? hub.provider_profiles : ['deepseek'];
    const field = (label, input, hint) => [
        el('label', { text: label, attrs: { for: input.id } }),
        input,
        hint ? el('span', { class: 'hint', text: hint }) : null,
    ];
    const sessionInput = el('input', { attrs: { type: 'text', id: 'ns-id', spellcheck: 'false' } });
    const providerSelect = el('select', { attrs: { id: 'ns-provider' } },
        profiles.map((name) => el('option', { text: name, attrs: { value: name } })));
    const modelInput = el('input', { attrs: { type: 'text', id: 'ns-model', placeholder: 'profile default' } });
    const threadsInput = el('input', { attrs: { type: 'number', id: 'ns-threads', min: '1', value: '1' } });
    const exchangesInput = el('input', { attrs: { type: 'number', id: 'ns-exchanges', min: '1', value: '512' } });
    const promptInput = el('input', { attrs: { type: 'text', id: 'ns-prompt', value: 'coding_agent.yaml' } });
    const workspaceInput = el('input', { attrs: { type: 'text', id: 'ns-workspace' } });
    const platformInput = el('input', { attrs: { type: 'text', id: 'ns-platform' } });
    const softwareInput = el('input', {
        attrs: { type: 'text', id: 'ns-software', placeholder: 'comma separated' },
    });
    const persistEnabled = el('input', { attrs: { type: 'checkbox', id: 'ns-persist' } });
    persistEnabled.checked = true;
    const persistReadable = el('input', { attrs: { type: 'checkbox', id: 'ns-readable' } });
    const restoreSelect = el('select', { attrs: { id: 'ns-restore' } }, [
        el('option', { text: 'if_present', attrs: { value: 'if_present' } }),
        el('option', { text: 'never', attrs: { value: 'never' } }),
    ]);
    const errorLine = el('p', { class: 'modal-error', attrs: { role: 'alert' } });
    const submit = el('button', {
        class: 'btn btn-primary',
        text: 'Create session',
        attrs: { type: 'submit' },
    });
    const form = el('form', { class: 'modal-body' }, [
        el('div', { class: 'form-grid' }, [
            ...field('session id', sessionInput, '1-128 characters: A-Z a-z 0-9 _ -'),
            ...field('provider profile', providerSelect),
            ...field('model override', modelInput, 'empty means the profile default'),
            ...field('threads', threadsInput),
            ...field('max exchanges', exchangesInput),
            ...field('system prompt file', promptInput, 'bare name resolves in the hub prompts directory'),
            ...field('workspace', workspaceInput),
            ...field('platform', platformInput),
            ...field('software hints', softwareInput),
            el('label', { text: 'persistence' }),
            el('div', { class: 'checkline' }, [
                el('label', {}, [persistEnabled, el('span', { text: ' enabled' })]),
                el('label', {}, [persistReadable, el('span', { text: ' readable export' })]),
            ]),
            ...field('restore policy', restoreSelect),
        ]),
        errorLine,
        el('div', { class: 'modal-foot' }, [
            el('span', { class: 'opt-note', text: 'spec fields left empty fall back to the hub defaults' }),
            el('span', { class: 'spacer' }),
            submit,
        ]),
    ]);
    form.addEventListener('submit', (event) => {
        event.preventDefault();
        const session = sessionInput.value.trim();
        if (!/^[A-Za-z0-9_-]{1,128}$/.test(session)) {
            errorLine.textContent = 'session id must be 1-128 characters of A-Z a-z 0-9 _ -';
            sessionInput.focus();
            return;
        }
        errorLine.textContent = '';
        const spec = {
            provider: providerSelect.value,
            persistence: { enabled: persistEnabled.checked, readable: persistReadable.checked },
            restore: restoreSelect.value,
        };
        const model = modelInput.value.trim();
        if (model) spec.model = model;
        const threads = Number.parseInt(threadsInput.value, 10);
        if (Number.isInteger(threads) && threads > 0) spec.threads = threads;
        const exchanges = Number.parseInt(exchangesInput.value, 10);
        if (Number.isInteger(exchanges) && exchanges > 0) spec.maxExchanges = exchanges;
        const promptFile = promptInput.value.trim();
        if (promptFile) spec.systemPromptFile = promptFile;
        const workspace = workspaceInput.value.trim();
        if (workspace) spec.workspace = workspace;
        const platform = platformInput.value.trim();
        if (platform) spec.platform = platform;
        const software = softwareInput.value.split(',').map((item) => item.trim()).filter(Boolean);
        if (software.length > 0) spec.software = software;
        submit.disabled = true;
        onSubmit?.({ session, spec });
    });
    return {
        element: form,
        sessionInput,
        submit,
        setError(message) {
            errorLine.textContent = message ?? '';
            submit.disabled = false;
        },
        setBusy() {
            submit.disabled = true;
        },
        read: () => ({ session: sessionInput.value.trim() }),
    };
}
