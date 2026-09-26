/**
 * @file what the browser tests share.
 *
 * Both spec files drive the same scripted hub and need the same three things:
 * a reset page that is connected before the test touches it, a way to push a
 * real event through the hub, and the payload builders for the events the
 * worker protocol defines. Keeping them here means the two suites cannot
 * disagree about what a `model_response` looks like.
 *
 * Not a `.spec.ts`, so Playwright collects it as a module rather than as a test
 * file with no tests in it.
 */
import { expect, type Page } from '@playwright/test';

/** Where the scripted hub listens; matches `playwright.config.ts`. */
export const STUB = `http://127.0.0.1:${process.env.STUB_HUB_PORT ?? 4180}`;

/** Reset the stub, then load the panel and wait for it to connect. */
export async function open(page: Page, query = ''): Promise<void> {
    await page.request.post(`${STUB}/__stub/reset`);
    await page.goto(`/app.html${query}`);
    await expect(page.getByText('connected', { exact: true })).toBeVisible();
}

/** Replace the stub's session list. */
export async function setSessions(page: Page, ids: string[]): Promise<void> {
    const response = await page.request.post(`${STUB}/__stub/sessions`, {
        data: {
            sessions: ids.map((id, index) => ({
                session_id: id,
                created_at: `2026-01-0${index + 1}T00:00:00.000Z`,
                spec: {},
                connected: true,
                identity: { state: 'live', worker_id: 'stub-worker', since: null },
                stats: { events: 0, gaps: 0, duplicates: 0, protocolErrors: 0, incarnations: 1 },
                last_run_id: '',
                last_event_at: null,
                last_event: null,
                confirmations: [],
                process: null,
                requests: [],
            })),
        },
    });
    expect(response.ok()).toBe(true);
}

/** Push one envelope through the stub. */
export async function emit(
    page: Page,
    event: string,
    data: unknown,
    extra: Record<string, unknown> = {},
): Promise<void> {
    const response = await page.request.post(`${STUB}/__stub/emit`, {
        data: { session: 'demo', event, data, extra },
    });
    expect(response.ok()).toBe(true);
}

/** A complete model response, as the worker protocol defines one. */
export function modelResponse(text: string, extra: Record<string, unknown> = {}) {
    return {
        type: 'model_response',
        role: 'assistant',
        content: [{ type: 'text', raw: text }],
        ...extra,
    };
}

/** One call object, as `tool_calls` and `model_response.invokes` carry it. */
export function call(id: string, name: string, args: unknown, extra: Record<string, unknown> = {}) {
    return { id, name, arguments: args, security: 'default_deny', type: 'read_only', ...extra };
}

/**
 * One `tool_results` entry.
 *
 * Deliberately in the shape core actually sends — a tool message with the
 * provenance nested under `invoke_return`, not the documented top-level
 * `{query, output}`. The panel has to read both.
 */
export function toolResult(id: string, name: string, text: string, extras?: unknown) {
    return {
        content: [{ type: 'text', raw: text }],
        invoke_return: {
            output: { type: 'text', raw: text },
            query: { id, name, arguments: {}, security: 'require_confirm', type: 'serial_write' },
        },
        role: 'tool',
        type: 'invoke_return',
        ...(extras === undefined ? {} : { extras }),
    };
}

/** The text a real `run_command` produced, fields and blocks and all. */
export const PROCESS_OUTPUT = [
    '[[session_id]]: proc_1',
    '[[state]]: exited',
    '[[exit_code]]: 0',
    '[[running_milliseconds]]: 12',
    '',
    'stdout (5 bytes):',
    'hello',
    '',
    'stderr (4 bytes):',
    'oops',
    '',
].join('\n');

/**
 * Run one whole turn through the stub: admission, a response proposing a call,
 * the batch, the result, and the boundary.
 */
export async function playTurn(
    page: Page,
    options: { text: string; command?: string; output?: string } = { text: 'hi' },
): Promise<void> {
    const command = options.command ?? 'echo hello';
    await emit(page, 'input_admitted', {}, { request_id: 'req-1' });
    await emit(page, 'run_started', {});
    if (options.text) await emit(page, 'model_response', modelResponse(options.text));
    await emit(page, 'tool_calls', [call('call-1', 'run_command', { command })]);
    await emit(page, 'tool_results', [
        toolResult('call-1', 'run_command', options.output ?? PROCESS_OUTPUT),
    ]);
    await emit(page, 'run_finished', { status: 'completed', exchanges: 1 });
}
