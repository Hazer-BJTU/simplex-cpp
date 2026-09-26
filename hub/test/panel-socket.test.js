import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { createPanelSocket } from '../web/src/lib/socket.ts';

class FakeWebSocket {
    static instances = [];
    readyState = 0;
    onopen = null;
    onmessage = null;
    onclose = null;
    onerror = null;

    constructor() {
        FakeWebSocket.instances.push(this);
    }

    open() {
        this.readyState = 1;
        this.onopen?.();
    }

    close() {
        this.readyState = 3;
        this.onclose?.();
    }

    send() {}
}

describe('panel socket replacement', () => {
    it('ignores callbacks from a socket superseded by a token change', () => {
        FakeWebSocket.instances.length = 0;
        const timers = [];
        const events = [];
        const socket = createPanelSocket({
            WebSocketImpl: FakeWebSocket,
            onEvent: (event) => events.push(event),
            setTimeoutImpl: (callback) => { timers.push(callback); return timers.length; },
            clearTimeoutImpl: () => {},
        });

        socket.connect();
        const old = FakeWebSocket.instances[0];
        old.open();
        socket.reconnectNow();
        const current = FakeWebSocket.instances[1];
        current.open();

        old.onclose();
        old.onmessage({ data: '{"v":1,"type":"pong"}' });
        assert.equal(socket.isOpen(), true);
        assert.equal(socket.state(), 'open');
        assert.equal(timers.length, 0);
        assert.equal(events.length, 0);
        assert.equal(socket.send({ type: 'ping' }), true);
        socket.close();
    });

    it('classifies each reconnect attempt using its own welcome state', () => {
        FakeWebSocket.instances.length = 0;
        const timers = [];
        const states = [];
        const socket = createPanelSocket({
            WebSocketImpl: FakeWebSocket,
            onState: (state) => states.push(state.state),
            setTimeoutImpl: (callback) => { timers.push(callback); return timers.length; },
            clearTimeoutImpl: () => {},
        });

        socket.connect();
        const first = FakeWebSocket.instances[0];
        first.open();
        first.onmessage({ data: '{"v":1,"type":"welcome"}' });
        first.close();
        assert.equal(states.at(-1), 'reconnecting');

        timers.shift()();
        FakeWebSocket.instances[1].close();
        assert.equal(states.at(-1), 'rejected');
        socket.close();
    });
});
