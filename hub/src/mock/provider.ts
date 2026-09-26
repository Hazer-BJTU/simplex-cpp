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
import type { IncomingMessage, Server, ServerResponse } from 'node:http';
import type { Logger } from '../log.ts';

/** Scenario names accepted by the configuration and the model-name mapping. */
export const SCENARIOS = ['auto', 'text', 'echo', 'slow', 'error'] as const;

/** One scripted model behaviour. */
export type Scenario = (typeof SCENARIOS)[number];

/** Default command proposed in the `auto` scenario. */
const DEFAULT_TOOL_COMMAND = "printf 'mock stdout\\n'; printf 'mock stderr\\n' >&2; "
    + 'touch mock-tool-marker.txt';

/** A resolved listen address. */
export interface MockAddress {
    host: string;
    port: number;
}

/**
 * Parse `host:port`, accepting bracketed IPv6 hosts.
 *
 * Duplicated from the CLI's parser on purpose: this module is a fixture and
 * should not drag the hub's argument handling into a configuration file's
 * dependency graph.
 */
export function parseAddress(text: unknown): MockAddress {
    const match = /^(?:\[(?<v6>[^\]]+)\]|(?<host>[^:]*)):(?<port>\d+)$/.exec(String(text).trim());
    if (!match) throw new Error(`mock.listen expects host:port, got "${String(text)}"`);
    return {
        host: (match.groups?.v6 ?? match.groups?.host) as string,
        port: Number.parseInt(match.groups?.port as string, 10),
    };
}

/** Map a requested model name onto a scenario. */
export function scenarioForModel(model: unknown, fallback: Scenario = 'auto'): Scenario {
    if (typeof model !== 'string' || !model.startsWith('mock-')) return fallback;
    const suffix = model.slice('mock-'.length);
    // `flash` and `tool` are aliases rather than scenarios: they name the model
    // a profile happens to use, not a behaviour.
    if (suffix === 'flash' || suffix === 'auto' || suffix === 'tool') return 'auto';
    return (SCENARIOS as readonly string[]).includes(suffix) ? (suffix as Scenario) : fallback;
}

/** Sleep helper. */
function delay(ms: number): Promise<void> {
    return new Promise((resolve) => {
        const timer = setTimeout(resolve, ms);
        timer.unref?.();
    });
}

/** A chat-completions request, as far as this fixture reads it. */
interface ChatRequest {
    model?: unknown;
    messages?: Array<{ role?: string; content?: unknown }>;
}

/** What one scenario answers with: text, or a tool call to propose. */
interface ScenarioDelta {
    text?: string;
    toolCall?: Record<string, unknown>;
}

/** Everything `MockProvider` needs. */
export interface MockProviderOptions {
    log: Logger;
    /** Fallback scenario for a model name that maps to nothing. */
    scenario?: Scenario;
    /** Delay used by the `slow` scenario. */
    slowMs?: number;
    /** Command proposed by `auto`. */
    toolCommand?: string;
    /** Its runtime hint. */
    toolExpectedRuntimeMs?: number;
}

/** Offline chat-completions server. */
export class MockProvider {
    readonly log: Logger;
    readonly scenario: Scenario;
    readonly slowMs: number;
    readonly toolCommand: string;
    readonly toolExpectedRuntimeMs: number;
    server: Server | null;
    baseUrl: string | null;
    /** Every request body received, newest last (bounded). */
    readonly requests: ChatRequest[];
    readonly failures: Error[];

    constructor({
        log,
        scenario = 'auto',
        slowMs = 1500,
        toolCommand = DEFAULT_TOOL_COMMAND,
        toolExpectedRuntimeMs = 1000,
    }: MockProviderOptions) {
        this.log = log;
        this.scenario = (SCENARIOS as readonly string[]).includes(scenario) ? scenario : 'auto';
        this.slowMs = slowMs;
        this.toolCommand = toolCommand;
        this.toolExpectedRuntimeMs = toolExpectedRuntimeMs;
        this.server = null;
        this.baseUrl = null;
        this.requests = [];
        this.failures = [];
    }

    /**
     * Bind the listener and record the resolved base URL.
     *
     * `advertiseHost` exists because those are two different questions. A hub
     * that puts workers in containers binds this on every interface and hands
     * them its bridge address, and a base URL of `0.0.0.0` is not something a
     * worker can connect to.
     */
    async start(
        { host = '127.0.0.1', port = 0 }: Partial<MockAddress> = {},
        { advertiseHost }: { advertiseHost?: string } = {},
    ): Promise<string> {
        this.server = createServer((req, res) => {
            void this.handle(req, res).catch((error: Error) => {
                this.failures.push(error);
                this.log.error(`mock provider request failed: ${error.message}`);
                if (!res.headersSent) {
                    res.writeHead(500, { 'Content-Type': 'application/json' });
                }
                res.end('{"error":"mock provider failure"}');
            });
        });
        const server = this.server;
        await new Promise<void>((resolve, reject) => {
            server.once('error', reject);
            server.listen(port, host, () => {
                server.removeListener('error', reject);
                resolve();
            });
        });
        const address = server.address();
        if (address === null || typeof address === 'string') {
            throw new Error('mock provider has no bound port');
        }
        const advertised = advertiseHost && advertiseHost.length > 0 ? advertiseHost : host;
        this.baseUrl = `http://${advertised.includes(':') ? `[${advertised}]` : advertised}:${address.port}`;
        this.log.info(`mock provider listening at ${this.baseUrl}`);
        return this.baseUrl;
    }

    /** Stop the listener. */
    async stop(): Promise<void> {
        if (!this.server) return;
        const server = this.server;
        this.server = null;
        this.baseUrl = null;
        await new Promise<void>((resolve) => {
            server.close(() => resolve());
            server.closeAllConnections?.();
        });
    }

    /** Read and parse a request body. */
    async readBody(req: IncomingMessage): Promise<ChatRequest> {
        const chunks: Buffer[] = [];
        for await (const chunk of req) chunks.push(chunk as Buffer);
        if (chunks.length === 0) return {};
        return JSON.parse(Buffer.concat(chunks).toString('utf8')) as ChatRequest;
    }

    /** Build the assistant delta for one request. */
    scenarioDelta(scenario: Scenario, body: ChatRequest): ScenarioDelta {
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
    async handle(req: IncomingMessage, res: ServerResponse): Promise<void> {
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
        const frames: object[] = [];
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
        const payload = `${frames.map((frame) => `data: ${JSON.stringify(frame)}\n\n`).join('')}data: [DONE]\n\n`;
        res.writeHead(200, {
            'Content-Type': 'text/event-stream',
            'Cache-Control': 'no-cache',
            'Content-Length': Buffer.byteLength(payload),
        });
        res.end(payload);
    }
}
