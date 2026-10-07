/** Repeated production-build measurements, deliberately not timing gates in CI. */
import { execFileSync } from 'node:child_process';
import { cpus, platform, arch } from 'node:os';
import { writeFile } from 'node:fs/promises';
import { expect, test } from '@playwright/test';
import { STUB, call, emit, modelResponse, toolResult } from './harness.ts';

test.skip(process.env.PANEL_BENCHMARK !== '1', 'Run with PANEL_BENCHMARK=1; timings are reports, not CI gates.');
test.use({ video: 'on', trace: 'on' });

test('large transcript, output burst, approvals and local pane switching', async ({ page }, testInfo) => {
    test.setTimeout(120_000);
    await page.request.post(`${STUB}/__stub/reset`);
    const turns = Number(process.env.PANEL_FIXTURE_TURNS ?? 320);
    const restored = Number(process.env.PANEL_HISTORY_TURNS ?? 0);
    const width = Number(process.env.PANEL_VIEWPORT_WIDTH ?? 1280);
    await page.setViewportSize({ width, height: 720 });
    const events = [];
    const code = Array.from({ length: 120 }, (_, index) => `const value${index} = ${index};`).join('\n');
    for (let index = 0; index < turns; index++) {
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
    if (restored) {
        await page.request.post(`${STUB}/__stub/settings`, { data: { historyEnabled: true,
            historyTurns: Array.from({ length: restored }, (_, index) => ({ index,
                user: [{ type: 'text', modality: 'text', raw: `Restored input ${index}` }],
                steps: [{ index: 0, content: [{ type: 'text', modality: 'text', raw: `Restored answer ${index}\n\n\`\`\`typescript\n${code}\n\`\`\`` }], tool_calls: 2 }],
                omitted_steps: 0,
            })),
        } });
        await emit(page, 'ready', { capabilities: ['session-history'] });
    }
    await page.goto('/?session=demo&panel_profile=1');
    // The ready/history handshake can add a non-execution prelude after the
    // fixture. Select the latest execution rather than the last section.
    await expect(page.locator('[data-testid="round"][data-kind="run"]').last()).toContainText(`Response ${turns - 1}`);
    if (restored) await expect(page.getByTestId('history-turn')).toHaveCount(restored);
    const profiler = await page.context().newCDPSession(page);
    await profiler.send('Profiler.enable');
    await profiler.send('Profiler.start');
    await page.evaluate(() => {
        const target = window as unknown as { __simplexPanelProfile: { reset(): void }; __benchmark: unknown };
        target.__simplexPanelProfile.reset();
        const state = { longTasks: [] as number[], gaps: [] as number[],
            interactions: [] as { name: string; duration: number; processingDelay: number; id: number }[], active: true };
        new PerformanceObserver(list => {
            for (const entry of list.getEntries() as PerformanceEventTiming[]) {
                if (entry.interactionId) state.interactions.push({ name: entry.name, duration: entry.duration,
                    processingDelay: entry.processingStart - entry.startTime, id: entry.interactionId });
            }
        }).observe({ type: 'event', durationThreshold: 16 } as PerformanceObserverInit);
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
    const burst = Array.from({ length: 80 }, (_, index) => ({
        event: 'model_response', data: modelResponse(`Burst message ${index}`),
        extra: { request_id: 'stress', run_id: 'stress-run' },
    }));
    await page.request.post(`${STUB}/__stub/emit-batch`, { data: { events: [
        { event: 'input_admitted', data: { operation: 'message' }, extra: { request_id: 'stress', run_id: 'stress-run' } },
        ...burst,
    ] } });
    await expect(page.getByTestId('transcript')).toContainText('Burst message 79');
    await page.request.post(`${STUB}/__stub/stream`, { data: { interval: 8,
        events: Array.from({ length: 120 }, (_, index) => ({ event: 'model_response',
            data: modelResponse(`Stream response ${index}`), extra: { request_id: 'stress', run_id: 'stress-run' } })),
    } });
    const typingPaint: number[] = [];
    const input = page.getByLabel('message', { exact: true });
    await input.focus();
    for (let index = 0; index < 10; index++) {
        await page.evaluate(() => {
            const target = window as unknown as { __inputLatency: number | undefined };
            target.__inputLatency = undefined;
            document.addEventListener('input', () => {
                const start = performance.now();
                requestAnimationFrame(() => requestAnimationFrame(() => {
                    target.__inputLatency = performance.now() - start;
                }));
            }, { once: true });
        });
        await page.keyboard.press('x');
        await page.waitForFunction(() => typeof (window as unknown as { __inputLatency?: number }).__inputLatency === 'number');
        typingPaint.push(await page.evaluate(() => (window as unknown as { __inputLatency: number }).__inputLatency));
    }
    await expect(page.getByTestId('transcript')).toContainText('Stream response 119');

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
    const { profile } = await profiler.send('Profiler.stop');
    await writeFile(testInfo.outputPath('panel.cpuprofile'), JSON.stringify(profile));
    await testInfo.attach('panel.cpuprofile', { body: JSON.stringify(profile), contentType: 'application/json' });
    const output = { fixture: { turns, events: turns * 6, restored, codeLines: 120, approvals: 8, burst: 81, stream: 120 },
        localPaint, typingPaint, report,
        revision: execFileSync('git', ['rev-parse', 'HEAD'], { encoding: 'utf8' }).trim(),
        dirty: execFileSync('git', ['status', '--porcelain'], { encoding: 'utf8' }).length > 0,
        host: { platform: platform(), arch: arch(), cpu: cpus()[0]?.model, node: process.version }, browser: page.context().browser()?.version(),
        build: 'Vite production', viewport: page.viewportSize(), cpuThrottling: 1 };
    await writeFile(testInfo.outputPath('panel-performance.json'), JSON.stringify(output, null, 2));
    await testInfo.attach('panel-performance.json', { body: JSON.stringify(output, null, 2), contentType: 'application/json' });
    console.log(JSON.stringify(output));
});
