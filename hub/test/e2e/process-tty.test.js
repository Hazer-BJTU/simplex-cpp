/** Real worker terminal sessions through Hub approvals and the panel parser. */
import assert from 'node:assert/strict';
import { rmSync } from 'node:fs';
import { it } from 'node:test';
import { e2eSkip, startE2eHub } from '../helpers/e2e.js';
import { connectWorker, until } from '../helpers/worker.js';
import { parseToolOutput } from '../../web/src/app/toolOutput.ts';

it('preserves terminal sessions across run cancellation and presents one merged output',
    { skip: e2eSkip, timeout: 60000 }, async t => {
        const ctx = await startE2eHub();
        const panel = await connectWorker(`${ctx.wsBase}/panel/ws`);
        t.after(async () => {
            await panel.close();
            await ctx.hub.stop();
            rmSync(ctx.config.dataDir, { recursive: true, force: true });
        });
        const session = ctx.hub.registry.create('terminal', { provider: 'mock', model: 'mock-text' });
        assert.equal((await ctx.hub.supervisor.start(session)).ok, true);
        await until(() => session.connected && session.workerCapabilities, { timeout: 20000 });
        panel.send({ v: 1, type: 'subscribe', session: session.id });
        await panel.waitFor(message => message.type === 'subscribed');

        let sequence = 0;
        async function call(name, args, { confirm = false, cancel = false } = {}) {
            const id = `tty-${++sequence}`;
            let requested = false;
            ctx.hub.mock.scenarioDelta = () => {
                if (requested) return { text: 'Tool operation finished.' };
                requested = true;
                return { toolCall: { index: 0, id, type: 'function',
                    function: { name, arguments: JSON.stringify(args) } } };
            };
            panel.send({ v: 1, type: 'input', session: session.id, request_id: id,
                content: [{ type: 'text', modality: 'text', raw: `Perform ${id}.` }] });
            if (confirm) {
                const prompt = await panel.waitFor(message => message.type === 'confirmation'
                    && message.open && message.confirmation.call.id === id);
                assert.equal(prompt.confirmation.verified, true);
                if ('tty' in args) assert.equal(prompt.confirmation.call.arguments.tty, args.tty);
                if (cancel) {
                    const started = await panel.waitFor(message => message.type === 'event'
                        && message.envelope.event === 'run_started' && message.envelope.request_id === id);
                    panel.send({ v: 1, type: 'signal', session: session.id,
                        operation: 'cancel', run_id: started.envelope.run_id });
                } else {
                    panel.send({ v: 1, type: 'confirmation', session: session.id,
                        confirmation_id: prompt.confirmation.confirmation_id, decision: 'approved' });
                }
            }
            const finished = await panel.waitFor(message => message.type === 'event'
                && message.envelope.event === 'run_finished' && message.envelope.request_id === id,
            { timeout: 20000 });
            assert.equal(finished.envelope.data.status, cancel ? 'cancelled' : 'completed');
            if (cancel) return;
            const result = panel.messages.find(message => message.type === 'event'
                && message.envelope.event === 'tool_results' && message.envelope.request_id === id);
            assert.ok(result, 'worker must report the tool result');
            return result.envelope.data[0].invoke_return.output.raw;
        }

        const launch = await call('spawn_process', { executable: '/bin/sh',
            arguments: ['-c', 'test -t 0 && test -t 1 && test -t 2 || exit 4; printf READY; cat; printf "\\033[31mTAIL\\033[0m\\n" >&2'],
            tty: true, expected_runtime_milliseconds: 0, auto_release: false }, { confirm: true });
        const id = launch.match(/\[\[session_id\]\]: (\S+)/)?.[1];
        assert.ok(id, launch);
        // Cancelling a separate run at its approval boundary must not tear down
        // the previously launched terminal session or deliver denied input.
        await call('send_process', { session_id: id, input: 'not-delivered\n' }, { confirm: true, cancel: true });
        await call('send_process', { session_id: id, input: 'hello\nlast', end_input: true }, { confirm: true });
        const status = await call('poll_process', { session_ids: [id], wait_timeout_milliseconds: 5000, include_output: false });
        assert.match(status, /\[\[output_complete\]\]: true/);
        const output = await call('read_process', { session_id: id });
        const parsed = parseToolOutput(output);
        assert.equal(parsed.kind, 'document', output);
        assert.equal(parsed.document.blocks.length, 1);
        assert.equal(parsed.document.blocks[0].name, 'output');
        const terminalText = 'READYhello\r\nlast\x1b[31mTAIL\x1b[0m\r\n';
        assert.ok(output.includes(terminalText), 'the wire result preserves terminal controls');
        // The existing prose parser removes the final block newline for display.
        assert.equal(parsed.document.blocks[0].text, terminalText.slice(0, -1));
        assert.match(output, /\[\[output_complete\]\]: true/);
        assert.match(output, /\[\[exit_code\]\]: 0/);
        const repeat = await call('read_process', { session_id: id, full: true, release: true });
        assert.equal(parseToolOutput(repeat).document.blocks[0].text, parsed.document.blocks[0].text);
        assert.match(repeat, /\[\[released\]\]: true/);
    });
