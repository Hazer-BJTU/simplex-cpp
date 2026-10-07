/** Repeated production-build measurements, deliberately not timing gates in CI. */
import { expect, test } from '@playwright/test';
import { STUB, call, emit, modelResponse, toolResult } from './harness.ts';

test.skip(process.env.PANEL_BENCHMARK !== '1', 'Run with PANEL_BENCHMARK=1; timings are reports, not CI gates.');
test.use({ video: 'on', trace: 'on' });

test('large transcript, output burst, approvals and local pane switching', async ({ page }, testInfo) => {
    test.setTimeout(120_000);
    await page.request.post(`${STUB}/__stub/reset`);
    const events = [];
    const code = Array.from({ length: 120 }, (_, index) => `const value${index} = ${index};`).join('\n');
    for (let index = 0; index < 320; index++) {
        const extra = { request_id: `load-request-${index}`, run_id: `load-run-${index}` };
        const text = `Response ${index}\n\n\`\`\`typescript\n${code}\n\`\`\``;
        events.push(
            { event: 'input_admitted', data: { operation: 'message' }, extra },
            { event: 'run_started', data: {}, extra },
            { event: 'model_response', data: modelResponse(text), extra },
            { event: 'tool_calls', data: [call(`load-call-${index}`, 'run_command', { command: 'echo test' })], extra },
            { event: 'tool_results', data: [toolResult(`load-call-${index}`, 'run_command', 'output\n'.repeat(400))], extra },
            { event: 'run_finished', data: { status: 'completed', exchanges: 1 }, extra },
        );
    }
    await page.request.post(`${STUB}/__stub/emit-batch`, { data: { events, seedOnly: true } });
    await page.request.post(`${STUB}/__stub/plan`, { data: { plan: { markdown: '# Plan\n\n- [ ] Work', revision: 1, updated_at: null } } });
    await page.goto('/?session=demo&panel_profile=1');
    await expect(page.getByTestId('round').last()).toContainText('Response 319');
    await page.evaluate(() => {
        const target = window as unknown as { __simplexPanelProfile: { reset(): void }; __benchmark: unknown };
        target.__simplexPanelProfile.reset();
        const state = { longTasks: [] as number[], gaps: [] as number[], active: true };
        new PerformanceObserver(list => state.longTasks.push(...list.getEntries().map(entry => entry.duration)))
            .observe({ type: 'longtask', buffered: false });
        let last = performance.now();
        function frame(now: number) {
            state.gaps.push(now - last);
            last = now;
            if (state.active) requestAnimationFrame(frame);
        }
        requestAnimationFrame(frame);
        target.__benchmark = state;
    });
    for (let index = 0; index < 8; index++) {
        await page.request.post(`${STUB}/__stub/confirm`, { data: {
            confirmation_id: `perf-${index}`, call: { name: 'run_command', arguments: { command: 'echo approve' } },
        } });
        await expect(page.getByRole('dialog')).toBeVisible();
        await page.keyboard.press('Escape');
    }
    const localPaint: number[] = [];
    for (let index = 0; index < 8; index++) {
        localPaint.push(await page.evaluate(async () => {
            const start = performance.now();
            (document.getElementById('plan-tab') as HTMLElement).click();
            await new Promise<void>(resolve => requestAnimationFrame(() => requestAnimationFrame(() => resolve())));
            return performance.now() - start;
        }));
        await page.getByRole('tab', { name: 'Conversation', exact: true }).click();
        await emit(page, 'tool_calls', [call(`burst-${index}`, 'read_text', { path: '/tmp/test' })]);
    }
    await page.getByTestId('approval-banner').first().getByRole('button', { name: 'Approve', exact: true }).click();
    await page.waitForTimeout(2000);
    await page.request.post(`${STUB}/__stub/settle`, { data: { confirmation_id: 'perf-0' } });
    const report = await page.evaluate(() => {
        const target = window as unknown as {
            __simplexPanelProfile: { read(): unknown };
            __benchmark: { longTasks: number[]; gaps: number[]; active: boolean };
        };
        target.__benchmark.active = false;
        return { counters: target.__simplexPanelProfile.read(), ...target.__benchmark,
            userAgent: navigator.userAgent, cores: navigator.hardwareConcurrency,
            domNodes: document.querySelectorAll('*').length,
            heap: (performance as unknown as { memory?: { usedJSHeapSize: number } }).memory?.usedJSHeapSize };
    });
    const output = { fixture: { turns: 320, events: 1920, codeLines: 120, approvals: 8 },
        localPaint, report, browser: page.context().browser()?.version(),
        build: 'Vite production', viewport: page.viewportSize(), cpuThrottling: 1 };
    await testInfo.attach('panel-performance.json', { body: JSON.stringify(output, null, 2), contentType: 'application/json' });
    console.log(JSON.stringify(output));
});
