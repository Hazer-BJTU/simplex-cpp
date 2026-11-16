import assert from 'node:assert/strict';
import { test } from 'node:test';
import { groupSessions, sessionRoot } from '../web/src/app/sessionGroups.ts';

function session(id, parent, created = id) {
    return {
        session_id: id, created_at: created, confirmations: [],
        ...(parent ? { kind: 'headless', subagent: { parent } } : {}),
    };
}

test('main order is independent of children; nested children share a flat root family', () => {
    const sessions = new Map([
        session('a', null, '1'), session('b', null, '2'),
        session('child', 'a', '4'), session('grandchild', 'child', '5'),
        session('other', 'b', '3'),
    ].map(value => [value.session_id, value]));
    sessions.get('grandchild').confirmations = [{ settled_at: null }, { settled_at: 'done' }];
    const result = groupSessions(sessions, 'child');
    assert.deepEqual(result.main.map(value => value.session_id), ['b', 'a']);
    assert.deepEqual(result.groups.map(value => value.root), ['a', 'b']);
    assert.deepEqual(result.groups[0].sessions.map(value => value.session_id), ['grandchild', 'child']);
    assert.equal(result.groups[0].pending, 1);
    assert.equal(groupSessions(sessions, 'b').groups[0].root, 'b');
});

test('missing parents and cycles remain visible and terminate traversal', () => {
    const sessions = new Map([
        session('orphan', 'missing'), session('x', 'y'), session('y', 'x'), session('a-prefix', 'x'),
    ].map(value => [value.session_id, value]));
    assert.equal(sessionRoot('orphan', sessions), 'missing');
    assert.equal(sessionRoot('x', sessions), sessionRoot('y', sessions));
    assert.equal(sessionRoot('a-prefix', sessions), sessionRoot('x', sessions));
    assert.equal(groupSessions(sessions, null).groups.flatMap(value => value.sessions).length, 4);
    assert.deepEqual(groupSessions(new Map(), null), { main: [], groups: [] });
});
