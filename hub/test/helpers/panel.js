/** @file isolated real Hub/peers with observable accepted panel sockets. */
import { rmSync } from 'node:fs';
import { WebSocketServer } from 'ws';
import { PANEL_VERSION } from '../../shared/protocol.ts';
import { startTestHub } from './hub.js';
import { connectWorker } from './worker.js';

export async function setupPanelHub(t, onPanel = () => {}) {
    const servers = [];
    const peers = [];
    const upgrade = WebSocketServer.prototype.handleUpgrade;
    t.mock.method(WebSocketServer.prototype, 'handleUpgrade', function (req, socket, head, callback) {
        return upgrade.call(this, req, socket, head, (ws, request) => {
            if (req.url === '/panel/ws') {
                servers.push(ws);
                onPanel(ws);
            }
            callback(ws, request);
        });
    });
    const ctx = await startTestHub();
    t.after(async () => {
        // A paused peer cannot complete a graceful close until it reads again.
        for (const peer of peers) peer.ws.terminate();
        for (const server of servers) server.terminate();
        try {
            await ctx.hub.stop();
        } finally {
            rmSync(ctx.config.dataDir, { recursive: true, force: true });
        }
    });
    async function connect(path) {
        const peer = await connectWorker(`${ctx.wsBase}${path}`);
        peers.push(peer);
        return peer;
    }
    async function panel() {
        const peer = await connect('/panel/ws');
        await peer.waitFor((message) => message.type === 'welcome');
        return { peer, server: servers.at(-1) };
    }
    async function subscribe(peer, session, since = 0) {
        peer.send({ v: PANEL_VERSION, type: 'subscribe', session, since });
        return peer.waitFor((message) => message.type === 'subscribed'
            && message.session.session_id === session);
    }
    return { ...ctx, connect, panel, subscribe, panelServers: servers };
}
