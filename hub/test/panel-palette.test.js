/**
 * @file what the command palette offers.
 *
 * The palette's entries are a pure function of the store's shape, so they are
 * checked here rather than by driving a browser. What matters is the shape of
 * the *offer*: an entry that cannot do anything should not be listed at all,
 * because a command that exists and then refuses is worse than one that is not
 * there.
 */
import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { buildCommands, filterCommands, matches } from '../web/src/app/palette.ts';

/** One session description, as the store holds it. */
function session(id, process = null) {
    return {
        session_id: id,
        created_at: '2026-01-01T00:00:00.000Z',
        spec: {},
        connected: process !== null,
        identity: { state: 'idle', worker_id: null, since: null },
        stats: { events: 0, gaps: 0, duplicates: 0, protocolErrors: 0, incarnations: 0 },
        last_run_id: '',
        last_event_at: null,
        last_event: null,
        confirmations: [],
        process,
        requests: [],
    };
}

/** The state the palette reads, with everything unremarkable by default. */
function input(overrides = {}) {
    return {
        sessions: [session('demo')],
        selected: 'demo',
        running: false,
        runActive: false,
        inspectorOpen: false,
        showDetails: false,
        confirmMode: 'ask',
        pingMs: null,
        ...overrides,
    };
}

/** The ids offered, for a compact assertion. */
function ids(state) {
    return buildCommands(state).map((command) => command.id);
}

describe('command palette', () => {
    it('offers one entry per session', () => {
        const commands = buildCommands(input({
            sessions: [session('alpha'), session('beta')],
            selected: 'alpha',
        }));
        const switches = commands.filter((command) => command.group === 'session');
        assert.deepEqual(switches.map((command) => command.label), [
            'Switch to alpha', 'Switch to beta',
        ]);
        assert.equal(switches[0].hint, 'current');
        assert.equal(switches[1].hint, undefined);
    });

    it('offers the opposite power action, not both', () => {
        const stopped = buildCommands(input({ running: false }))
            .find((command) => command.id === 'process:power');
        assert.equal(stopped.label, 'Start the worker');
        assert.deepEqual(stopped.action, { kind: 'worker', session: 'demo', action: 'start' });

        const running = buildCommands(input({ running: true }))
            .find((command) => command.id === 'process:power');
        assert.equal(running.label, 'Stop the worker');
        assert.equal(running.action.action, 'stop');
    });

    it('does not offer to cancel a run that is not happening', () => {
        assert.ok(!ids(input({ runActive: false })).includes('process:cancel'));
        assert.ok(ids(input({ runActive: true })).includes('process:cancel'));
    });

    it('offers nothing about a session when none is selected', () => {
        const offered = ids(input({ selected: null }));
        assert.deepEqual(offered, ['session:demo', 'hub:ping', 'view:details']);
    });

    it('describes the drawer by what it will do, not what it is', () => {
        const closed = buildCommands(input({ inspectorOpen: false }))
            .find((command) => command.id === 'view:inspector');
        assert.equal(closed.label, 'Show the context drawer');
        const open = buildCommands(input({ inspectorOpen: true }))
            .find((command) => command.id === 'view:inspector');
        assert.equal(open.label, 'Hide the context drawer');
    });

    it('says which way the technical-details switch will go', () => {
        const off = buildCommands(input()).find((command) => command.id === 'view:details');
        assert.equal(off.label, 'Show technical details');
        const on = buildCommands(input({ showDetails: true }))
            .find((command) => command.id === 'view:details');
        assert.equal(on.label, 'Hide technical details');
    });

    it('reports the last round trip beside the heartbeat', () => {
        const unknown = buildCommands(input()).find((command) => command.id === 'hub:ping');
        assert.equal(unknown.hint, undefined);
        const timed = buildCommands(input({ pingMs: 7 })).find((c) => c.id === 'hub:ping');
        assert.equal(timed.hint, '7ms');
    });
});

describe('command filtering', () => {
    it('matches characters in order, not just substrings', () => {
        assert.equal(matches('Switch to alpha', 'swa'), true);
        assert.equal(matches('Switch to alpha', 'zzz'), false);
        assert.equal(matches('anything', ''), true);
    });

    it('filters on the hint as well as the label', () => {
        const commands = buildCommands(input({
            sessions: [session('alpha'), session('beta')],
            selected: 'alpha',
        }));
        const found = filterCommands(commands, 'beta');
        assert.deepEqual(found.map((command) => command.id), ['session:beta']);
    });

    it('returns nothing rather than everything for a query nothing matches', () => {
        const commands = buildCommands(input());
        assert.deepEqual(filterCommands(commands, 'qqqq'), []);
    });
});
