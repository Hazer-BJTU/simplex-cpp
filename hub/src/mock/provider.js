/**
 * @file optional offline model provider.
 *
 * The hub can serve a chat-completions endpoint that speaks the same
 * server-sent-events dialect the bundled DeepSeek adapter expects, with scripted
 * scenarios. That makes the whole chain — panel, hub, worker, tool dispatch,
 * confirmation, persistence — clickable without an API key, and it gives the
 * end-to-end test a model that behaves deterministically.
 *
 * The scenario is selected by the model name the worker asks for, so a session
 * chooses one through its ordinary provider profile:
 *
 *   mock-flash / mock-auto  two-step: propose `run_command`, then finish
 *   mock-text               reply with text only
 *   mock-echo               reply with the last user message
 *   mock-slow               wait before replying (cancellation testing)
 *   mock-error              fail the request
 *
 * This is a test and demo fixture. It is never started unless `mock.enabled` is
 * true, and it deliberately implements no authentication.
 */
import { randomUUID } from 'node:crypto';
import { createServer } from 'node:http';

/** Scenario names accepted by the configuration and the model-name mapping. */
export const SCENARIOS = ['auto', 'text', 'echo', 'slow', 'error'];

/** Default command proposed in the `auto` scenario. */
const DEFAULT_TOOL_COMMAND = "printf 'mock stdout\\n'; printf 'mock stderr\\n' >&2; "
    + 'touch mock-tool-marker.txt';

/** Parse `host:port`, accepting bracketed IPv6 hosts. */
export function parseAddress(text) {
    const match = /^(?:\[(?<v6>[^\]]+)\]|(?<host>[^:]*)):(?<port>\d+)$/.exec(String(text).trim());
    if (!match) throw new Error(`mock.listen expects host:port, got "${text}"`);
    return {
        host: match.groups.v6 ?? match.groups.host,
        port: Number.parseInt(match.groups.port, 10),
    };
}

/** Map a requested model name onto a scenario. */
export function scenarioForModel(model, fallback = 'auto') {
    if (typeof model !== 'string' || !model.startsWith('mock-')) return fallback;
    const suffix = model.slice('mock-'.length);
    if (suffix === 'flash' || suffix === 'auto' || suffix === 'tool') return 'auto';
    return SCENARIOS.includes(suffix) ? suffix : fallback;
}

/** Sleep helper. */
function delay(ms) {
    return new Promise((resolve) => {
        const timer = setTimeout(resolve, ms);
        timer.unref?.();
    });
}

/** Offline chat-completions server. */
export class MockProvider {
    /**
     * @param {object} options
     * @param {object} options.log hub logger.
     * @param {string} [options.scenario] fallback scenario for unknown models.
     * @param {number} [options.slowMs] delay used by the `slow` scenario.
     * @param {string} [options.toolCommand] command proposed by `auto`.
     * @param {number} [options.toolExpectedRuntimeMs] its runtime hint.
     */
    constructor({
        log,
        scenario = 'auto',
        slowMs = 1500,
        toolCommand = DEFAULT_TOOL_COMMAND,
        toolExpectedRuntimeMs = 1000,
    }) {
        this.log = log;
        this.scenario = SCENARIOS.includes(scenario) ? scenario : 'auto';
        this.slowMs = slowMs;
        this.toolCommand = toolCommand;
        this.toolExpectedRuntimeMs = toolExpectedRuntimeMs;
        this.server = null;
        this.baseUrl = null;
        /** Every request body received, newest last (bounded). */
        this.requests = [];
        this.failures = [];
    }

    /** Bind the listener and record the resolved base URL. */
    async start({ host = '127.0.0.1', port = 0 } = {}) {
        this.server = createServer((req, res) => {
            void this.handle(req, res).catch((error) => {
                this.failures.push(error);
                this.log.error(`mock provider request failed: ${error.message}`);
                if (!res.headersSent) {
                    res.writeHead(500, { 'Content-Type': 'application/json' });
                }
                res.end('{"error":"mock provider failure"}');
            });
        });
        await new Promise((resolve, reject) => {
            this.server.once('error', reject);
            this.server.listen(port, host, () => {
                this.server.removeListener('error', reject);
                resolve();
            });
        });
        const address = this.server.address();
        this.baseUrl = `http://${host}:${address.port}`;
        this.log.info(`mock provider listening at ${this.baseUrl}`);
        return this.baseUrl;
    }

    /** Stop the listener. */
    async stop() {
        if (!this.server) return;
        const server = this.server;
        this.server = null;
        this.baseUrl = null;
        await new Promise((resolve) => {
            server.close(resolve);
            server.closeAllConnections?.();
        });
    }

    /** Read and parse a request body. */
    async readBody(req) {
        const chunks = [];
        for await (const chunk of req) chunks.push(chunk);
        if (chunks.length === 0) return {};
        return JSON.parse(Buffer.concat(chunks).toString('utf8'));
    }

    /** Build the assistant delta for one request. */
    scenarioDelta(scenario, body) {
        const messages = Array.isArray(body.messages) ? body.messages : [];
        const lastUser = [...messages].reverse().find((message) => message.role === 'user');
        switch (scenario) {
            case 'echo': {
                const text = typeof lastUser?.content === 'string'
                    ? lastUser.content
                    : JSON.stringify(lastUser?.content ?? '');
                return { text: `echo: ${text}` };
            }
            case 'auto': {
                const toolResults = messages.filter((message) => message.role === 'tool');
                if (toolResults.length === 0) {
                    return {
                        toolCall: {
                            index: 0,
                            id: 'mock-call-1',
                            type: 'function',
                            function: {
                                name: 'run_command',
                                arguments: JSON.stringify({
                                    command: this.toolCommand,
                                    expected_runtime_milliseconds: this.toolExpectedRuntimeMs,
                                }),
                            },
                        },
                    };
                }
                return { text: `finished after ${toolResults.length} tool result(s)` };
            }
            case 'slow':
            case 'text':
            default:
                return { text: 'mock response' };
        }
    }

    /** Serve one chat-completions request. */
    async handle(req, res) {
        if (req.method !== 'POST') {
            res.writeHead(405, { 'Content-Type': 'application/json' });
            res.end('{"error":"POST only"}');
            return;
        }
        const body = await this.readBody(req);
        this.requests.push(body);
        if (this.requests.length > 50) this.requests.shift();
        const scenario = scenarioForModel(body.model, this.scenario);

        if (scenario === 'error') {
            res.writeHead(500, { 'Content-Type': 'application/json' });
            res.end('{"error":{"message":"mock provider scenario error"}}');
            return;
        }
        if (scenario === 'slow') await delay(this.slowMs);

        const delta = this.scenarioDelta(scenario, body);
        const frames = [];
        if (delta.toolCall) {
            frames.push({
                id: randomUUID(),
                choices: [{
                    index: 0,
                    delta: { role: 'assistant', tool_calls: [delta.toolCall] },
                    finish_reason: null,
                }],
            });
            frames.push({
                id: randomUUID(),
                choices: [{ index: 0, delta: {}, finish_reason: 'tool_calls' }],
            });
        } else {
            frames.push({
                id: randomUUID(),
                choices: [{
                    index: 0,
                    delta: { role: 'assistant', content: delta.text },
                    finish_reason: null,
                }],
            });
            frames.push({
                id: randomUUID(),
                choices: [{ index: 0, delta: {}, finish_reason: 'stop' }],
            });
        }
        const body_ = `${frames.map((frame) => `data: ${JSON.stringify(frame)}\n\n`).join('')}data: [DONE]\n\n`;
        res.writeHead(200, {
            'Content-Type': 'text/event-stream',
            'Cache-Control': 'no-cache',
            'Content-Length': Buffer.byteLength(body_),
        });
        res.end(body_);
    }
}
