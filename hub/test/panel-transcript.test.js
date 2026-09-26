/**
 * @file reading a tool result's prose, and folding a transcript into rounds.
 *
 * Both modules are pure functions of what the worker sent, which is why they
 * are checked here rather than in a browser. The cases that matter are the ones
 * where the panel previously got it wrong:
 *
 *   - a result in the shape core actually sends, not only the documented one;
 *   - `tool_calls` and `model_response.invokes` describing one batch, which the
 *     old panel drew twice;
 *   - an event name this build has never seen, which must lose nothing.
 */
import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { parseToolOutput } from '../web/src/app/toolOutput.ts';
import { buildRounds } from '../web/src/app/rounds.ts';
import { fenceFor } from '../web/src/app/content.ts';

/** One transcript event item, as the store builds it. */
function event(id, eventName, data, extra = {}) {
    const ordinal = Number(id.replace(/\D/g, '')) || 1;
    return {
        kind: 'event',
        id,
        epoch: 'e1',
        envelope: {
            type: 'event',
            event: eventName,
            session_id: 'demo',
            worker_id: 'w1',
            request_id: extra.request_id ?? 'req-1',
            run_id: extra.run_id ?? 'run-1',
            sequence: extra.sequence ?? ordinal,
            data,
            hub_sequence: extra.hub_sequence ?? ordinal,
            received_at: extra.received_at ?? '2026-01-01T00:00:00.000Z',
        },
    };
}

/** A model response carrying the given text and calls. */
function response(id, text, invokes, extra = {}) {
    return event(id, 'model_response', {
        type: 'model_response',
        role: 'assistant',
        content: [{ type: 'text', raw: text }],
        ...(invokes ? { invokes } : {}),
    }, extra);
}

/** One call object, as `tool_calls` and `invokes` carry it. */
function call(id, name, args, extra = {}) {
    return { id, name, arguments: args, security: 'default_deny', type: 'read_only', ...extra };
}

/**
 * The text a real `run_command` returned, copied from a live session:
 * `[[field]]: value` lines, then named output blocks.
 */
const REAL_OUTPUT = [
    '[[session_id]]: proc_e5203993_1',
    '[[state]]: exited',
    '[[exit_code]]: 0',
    '[[running_milliseconds]]: 1',
    '[[finished]]: true',
    '',
    'stdout (12 bytes):',
    'mock stdout',
    '',
    'stderr (12 bytes):',
    'mock stderr',
    '',
].join('\n');

/** A `tool_results` entry in the shape core actually sends. */
function realResult(callId, name, text) {
    return {
        content: [{ type: 'text', raw: text }],
        invoke_return: {
            output: { type: 'text', raw: text },
            query: { id: callId, name, arguments: {}, security: 'require_confirm', type: 'serial_write' },
        },
        role: 'tool',
        type: 'invoke_return',
    };
}

describe('tool output', () => {
    it('reads the fields and blocks a process tool renders', () => {
        const output = parseToolOutput(REAL_OUTPUT);
        assert.equal(output.kind, 'document');
        const { fields, blocks, reportedMs } = output.document;
        assert.deepEqual(fields.map((field) => field.name), [
            'session_id', 'state', 'exit_code', 'running_milliseconds', 'finished',
        ]);
        assert.deepEqual(blocks.map((block) => block.name), ['stdout', 'stderr']);
        assert.equal(blocks[0].text, 'mock stdout');
        assert.equal(blocks[1].text, 'mock stderr');
        assert.equal(blocks[0].bytes, 12);
        assert.equal(reportedMs, 1);
    });

    it('keeps a block body that contains blank lines', () => {
        const output = parseToolOutput('stdout (6 bytes):\none\n\ntwo\n\nstderr: (empty)\n');
        assert.equal(output.kind, 'document');
        assert.equal(output.document.blocks[0].text, 'one\n\ntwo');
        assert.equal(output.document.blocks[1].empty, true);
    });

    it('notes a truncated block as truncated', () => {
        const output = parseToolOutput('stdout (truncated, first 4000 bytes):\nabc\n');
        assert.equal(output.kind, 'document');
        assert.equal(output.document.blocks[0].truncated, true);
        assert.equal(output.document.blocks[0].bytes, 4000);
    });

    it('falls back to plain text for a tool that does not use the format', () => {
        const output = parseToolOutput('just some prose\nover two lines\n');
        assert.equal(output.kind, 'text');
        assert.match(output.text, /just some prose/);
    });

    it('keeps the unrecognised remainder instead of dropping it', () => {
        const output = parseToolOutput('[[state]]: exited\n\nstdout (3 bytes):\nabc\nnot a block\n');
        assert.equal(output.kind, 'document');
        // The trailing line is not a header and is not part of `abc`, so it is
        // surfaced rather than swallowed.
        assert.deepEqual(output.document.rest, []);
        assert.equal(output.document.blocks[0].text, 'abc\nnot a block');
    });

    it('reports no runtime when the tool did not report one', () => {
        const output = parseToolOutput('stdout (3 bytes):\nabc\n');
        assert.equal(output.kind, 'document');
        assert.equal(output.document.reportedMs, null);
    });
});

describe('rounds', () => {
    const noPrompts = new Map();

    it('groups one turn into a single round', () => {
        const items = [
            event('e1', 'ready', {}),
            event('e2', 'status', { active: false }),
            event('e3', 'input_admitted', {}),
            event('e4', 'run_started', {}),
            response('e5', 'hello'),
            event('e6', 'run_finished', { status: 'completed', exchanges: 1 }),
        ];
        const rounds = buildRounds(items, noPrompts);
        assert.equal(rounds.length, 2, 'expected the prelude and one turn');
        assert.equal(rounds[0].kind, 'prelude');
        assert.equal(rounds[0].protocol.length, 2);
        assert.equal(rounds[1].kind, 'run');
        assert.equal(rounds[1].index, 1);
        assert.equal(rounds[1].status, 'completed');
        assert.equal(rounds[1].exchanges, 1);
        assert.equal(rounds[1].assistant.length, 1);
        assert.equal(rounds[1].assistant[0].text, 'hello');
        assert.equal(rounds[1].open, false);
    });

    it('draws one card for a batch the response and the event both describe', () => {
        // Core sends the calls inside the response and then a `tool_calls`
        // event for the same batch. The old panel drew a card for each.
        const proposed = call('mock-call-1', 'run_command', { command: 'ls' });
        const items = [
            event('e1', 'input_admitted', {}),
            response('e2', '', [proposed]),
            event('e3', 'tool_calls', [proposed]),
            event('e4', 'run_finished', { status: 'completed' }),
        ];
        const rounds = buildRounds(items, noPrompts);
        const run = rounds.at(-1);
        assert.equal(run.calls.length, 1);
        assert.equal(run.calls[0].name, 'run_command');
        // And it is drawn where the response proposed it, not twice.
        assert.deepEqual(run.assistant[0].callIds, [run.calls[0].key]);
        assert.equal(run.timeline.filter((entry) => entry.kind === 'calls').length, 0);
    });

    it('keeps a call that only the batch mentions', () => {
        const items = [
            event('e1', 'input_admitted', {}),
            event('e2', 'tool_calls', [call('c-9', 'send_process', {})]),
            event('e3', 'run_finished', { status: 'completed' }),
        ];
        const rounds = buildRounds(items, noPrompts);
        const run = rounds.at(-1);
        assert.equal(run.calls.length, 1);
        assert.equal(run.timeline.filter((entry) => entry.kind === 'calls').length, 1);
    });

    it('reads a result in the shape core sends, not only the documented one', () => {
        const items = [
            event('e1', 'input_admitted', {}),
            response('e2', '', [call('c1', 'run_command', { command: 'ls' })]),
            event('e3', 'tool_results', [realResult('c1', 'run_command', REAL_OUTPUT)]),
            event('e4', 'run_finished', { status: 'completed' }),
        ];
        const rounds = buildRounds(items, noPrompts);
        const tool = rounds.at(-1).calls[0];
        assert.equal(tool.name, 'run_command');
        assert.equal(tool.status, 'ok');
        assert.equal(tool.output.kind, 'document');
        assert.equal(tool.reportedMs, 1);
        // The settled classification wins: the proposed one is not authoritative.
        assert.equal(tool.security, 'require_confirm');
    });

    it('calls a result a failure only when the tool framework said so', () => {
        const failed = {
            ...realResult('c1', 'run_command', 'stdout: (empty)\n'),
            extras: { error: { stage: 'invoke', message: 'the process could not start' } },
        };
        const items = [
            event('e1', 'input_admitted', {}),
            response('e2', '', [call('c1', 'run_command', {})]),
            event('e3', 'tool_results', [failed]),
            event('e4', 'run_finished', { status: 'completed' }),
        ];
        const tool = buildRounds(items, noPrompts).at(-1).calls[0];
        assert.equal(tool.status, 'failed');
        assert.equal(tool.result.error.stage, 'invoke');
    });

    it('distinguishes a call that was not run from one that failed', () => {
        const skipped = {
            ...realResult('c1', 'run_command', ''),
            extras: { loop_skipped: true },
        };
        const items = [
            event('e1', 'input_admitted', {}),
            response('e2', '', [call('c1', 'run_command', {})]),
            event('e3', 'tool_results', [skipped]),
            event('e4', 'run_finished', { status: 'completed' }),
        ];
        assert.equal(buildRounds(items, noPrompts).at(-1).calls[0].status, 'skipped');
    });

    it('marks a call with no result once the run has finished', () => {
        const items = [
            event('e1', 'input_admitted', {}),
            response('e2', '', [call('c1', 'run_command', {})]),
            event('e3', 'run_finished', { status: 'completed' }),
        ];
        assert.equal(buildRounds(items, noPrompts).at(-1).calls[0].status, 'unknown');
    });

    it('gives a call its open approval', () => {
        const prompt = {
            confirmation_id: 'conf-1',
            session_id: 'demo',
            worker_id: 'w1',
            run_id: 'run-1',
            state: 'awaiting-decision',
            verified: true,
            identity_state: 'live',
            call: { id: 'c1', name: 'run_command', arguments: { command: 'ls' } },
            received_at: '2026-01-01T00:00:00.000Z',
            deadline_at: null,
            settled_at: null,
            decision: null,
            reason: null,
        };
        const items = [
            event('e1', 'input_admitted', {}),
            response('e2', '', [call('c1', 'run_command', { command: 'ls' })]),
        ];
        const tool = buildRounds(items, new Map([['conf-1', prompt]])).at(-1).calls[0];
        assert.equal(tool.status, 'pending');
        assert.equal(tool.prompt.confirmation_id, 'conf-1');
    });

    it('does not hand one call\'s approval to another call of the same tool', () => {
        const prompt = {
            confirmation_id: 'conf-1', session_id: 'demo', worker_id: 'w1', run_id: 'run-1',
            state: 'awaiting-decision', verified: true, identity_state: 'live',
            call: { id: 'c1', name: 'run_command', arguments: {} },
            received_at: '2026-01-01T00:00:00.000Z', deadline_at: null,
            settled_at: null, decision: null, reason: null,
        };
        const items = [
            event('e1', 'input_admitted', {}),
            response('e2', '', [
                call('c1', 'run_command', {}),
                call('c2', 'run_command', {}),
            ]),
        ];
        const calls = buildRounds(items, new Map([['conf-1', prompt]])).at(-1).calls;
        assert.equal(calls[0].status, 'pending');
        assert.equal(calls[1].status, 'running');
    });

    it('starts a new round for a second admission', () => {
        const items = [
            event('e1', 'input_admitted', {}),
            response('e2', 'first'),
            event('e3', 'run_finished', { status: 'completed' }),
            event('e4', 'input_admitted', {}),
            response('e5', 'second'),
            event('e6', 'run_finished', { status: 'completed' }),
        ];
        const rounds = buildRounds(items, noPrompts);
        assert.equal(rounds.length, 2);
        assert.deepEqual(rounds.map((round) => round.index), [1, 2]);
        assert.equal(rounds[1].assistant[0].text, 'second');
    });

    it('keeps an unfamiliar event rather than losing part of the turn', () => {
        // Core is allowed to add event names; a panel that dropped one would
        // silently shorten the conversation.
        const items = [
            event('e1', 'input_admitted', {}),
            event('e2', 'invented_by_a_newer_core', { anything: true }),
            response('e3', 'done'),
            event('e4', 'run_finished', { status: 'completed' }),
        ];
        const run = buildRounds(items, noPrompts).at(-1);
        assert.equal(run.problems.length, 1);
        assert.equal(run.problems[0].label, 'invented_by_a_newer_core');
        assert.equal(run.assistant[0].text, 'done');
    });

    it('sums token costs while leaving an unreported cost as unknown', () => {
        const costing = (id, prompt, generated) => event(id, 'model_response', {
            type: 'model_response',
            role: 'assistant',
            content: [{ type: 'text', raw: 'x' }],
            cost: { prompt, generated, cache_hit: 5 },
        });
        const items = [
            event('e1', 'input_admitted', {}),
            costing('e2', 100, 10),
            costing('e3', 50, 5),
            event('e4', 'run_finished', { status: 'completed' }),
            event('e5', 'input_admitted', {}),
            response('e6', 'no cost reported'),
            event('e7', 'run_finished', { status: 'completed' }),
        ];
        const rounds = buildRounds(items, noPrompts);
        // `cache_hit` is already inside `prompt`, so it is not added again.
        assert.equal(rounds[0].tokens, 165);
        assert.equal(rounds[1].tokens, null);
    });

    it('records the order things happened in', () => {
        const items = [
            event('e1', 'input_admitted', {}),
            event('e2', 'run_started', {}),
            response('e3', 'thinking'),
            event('e4', 'persisted', { boundary: 'before_tools' }),
            event('e5', 'run_finished', { status: 'completed' }),
        ];
        const run = buildRounds(items, noPrompts).at(-1);
        assert.deepEqual(run.timeline.map((entry) => entry.kind), [
            'protocol', 'protocol', 'assistant', 'protocol', 'protocol',
        ]);
    });
});

describe('markdown fences', () => {
    it('grows past the longest run of backticks in the content', () => {
        // A command containing ``` would otherwise close the block and the rest
        // of it would be parsed as markdown — untrusted text becoming structure.
        assert.equal(fenceFor('echo hi'), '```');
        assert.equal(fenceFor('echo ```; rm -rf /'), '````');
        assert.equal(fenceFor('echo ````'), '`````');
    });
});
