/**
 * @file the offline mock provider: scenario selection and the wire shape the
 * bundled DeepSeek adapter expects.
 */
import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { MockProvider, SCENARIOS, parseAddress, scenarioForModel } from '../src/mock/provider.ts';
import { createLogger } from '../src/log.ts';

const log = createLogger({ level: 'silent' });

/** Post a chat-completions request and return the decoded SSE frames. */
async function complete(baseUrl, body) {
    const response = await fetch(`${baseUrl}/chat/completions`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
    });
    const text = await response.text();
    const frames = text.split('\n\n')
        .filter((chunk) => chunk.startsWith('data: ') && chunk !== 'data: [DONE]')
        .map((chunk) => JSON.parse(chunk.slice('data: '.length)));
    return { status: response.status, frames, text };
}

describe('mock scenario selection', () => {
    it('maps model names onto scenarios', () => {
        assert.equal(scenarioForModel('mock-flash'), 'auto');
        assert.equal(scenarioForModel('mock-auto'), 'auto');
        assert.equal(scenarioForModel('mock-tool'), 'auto');
        assert.equal(scenarioForModel('mock-text'), 'text');
        assert.equal(scenarioForModel('mock-slow'), 'slow');
        assert.equal(scenarioForModel('mock-error'), 'error');
        assert.equal(scenarioForModel('mock-unknown'), 'auto');
        assert.equal(scenarioForModel('deepseek-flash'), 'auto');
        assert.equal(scenarioForModel(undefined), 'auto');
        assert.equal(scenarioForModel('mock-anything', 'text'), 'text');
    });

    it('exposes the scenario names', () => {
        assert.deepEqual(SCENARIOS, ['auto', 'text', 'echo', 'slow', 'error']);
    });

    it('parses listen addresses', () => {
        assert.deepEqual(parseAddress('127.0.0.1:0'), { host: '127.0.0.1', port: 0 });
        assert.deepEqual(parseAddress('[::1]:8801'), { host: '::1', port: 8801 });
        assert.throws(() => parseAddress('nonsense'), /host:port/);
    });
});

describe('mock provider', () => {
    it('proposes a tool call first and finishes after results arrive', async () => {
        const mock = new MockProvider({ log, toolCommand: 'touch marker.txt' });
        const baseUrl = await mock.start();
        try {
            const first = await complete(baseUrl, {
                model: 'mock-flash',
                messages: [{ role: 'user', content: 'go' }],
            });
            assert.equal(first.status, 200);
            assert.equal(first.frames.length, 2);
            const call = first.frames[0].choices[0].delta.tool_calls[0];
            assert.equal(call.function.name, 'run_command');
            assert.deepEqual(JSON.parse(call.function.arguments), {
                command: 'touch marker.txt',
                expected_runtime_milliseconds: 1000,
            });
            assert.equal(first.frames[1].choices[0].finish_reason, 'tool_calls');
            assert.match(first.text, /data: \[DONE\]/);

            const second = await complete(baseUrl, {
                model: 'mock-flash',
                messages: [
                    { role: 'user', content: 'go' },
                    { role: 'tool', content: 'mock stdout' },
                ],
            });
            assert.equal(second.frames[0].choices[0].delta.content, 'finished after 1 tool result(s)');
            assert.equal(second.frames[1].choices[0].finish_reason, 'stop');
            assert.equal(mock.requests.length, 2);
        } finally {
            await mock.stop();
        }
    });

    it('streams plain text for the text and echo scenarios', async () => {
        const mock = new MockProvider({ log });
        const baseUrl = await mock.start();
        try {
            const text = await complete(baseUrl, {
                model: 'mock-text', messages: [{ role: 'user', content: 'hi' }],
            });
            assert.equal(text.frames[0].choices[0].delta.content, 'mock response');
            assert.equal(text.frames[0].choices[0].delta.role, 'assistant');

            const echo = await complete(baseUrl, {
                model: 'mock-echo', messages: [{ role: 'user', content: 'ping' }],
            });
            assert.equal(echo.frames[0].choices[0].delta.content, 'echo: ping');
        } finally {
            await mock.stop();
        }
    });

    it('fails the request in the error scenario', async () => {
        const mock = new MockProvider({ log });
        const baseUrl = await mock.start();
        try {
            const response = await fetch(`${baseUrl}/chat/completions`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ model: 'mock-error', messages: [] }),
            });
            assert.equal(response.status, 500);
            assert.match(await response.text(), /scenario error/);
        } finally {
            await mock.stop();
        }
    });

    it('rejects a non-POST request', async () => {
        const mock = new MockProvider({ log });
        const baseUrl = await mock.start();
        try {
            const response = await fetch(`${baseUrl}/chat/completions`);
            assert.equal(response.status, 405);
        } finally {
            await mock.stop();
        }
    });

    it('ignores a request body it cannot parse without disturbing the server', async () => {
        const mock = new MockProvider({ log });
        const baseUrl = await mock.start();
        try {
            const response = await fetch(`${baseUrl}/chat/completions`, { method: 'POST', body: '{' });
            assert.equal(response.status, 500);
            assert.equal(mock.failures.length, 1);
            const healthy = await complete(baseUrl, { model: 'mock-text', messages: [] });
            assert.equal(healthy.status, 200);
        } finally {
            await mock.stop();
        }
    });
});
