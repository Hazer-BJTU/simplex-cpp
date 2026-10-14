/** Browser-owned entry point. Serve behind the Hub's same-origin reverse proxy. */
import { discover, observe } from '../observer.mjs';

/** Mount a text-only viewer. All untrusted content uses textContent, never HTML. */
export function mountViewer(document, base = location.origin, socket = WebSocket) {
    const token = document.querySelector('#token');
    const session = document.querySelector('#session');
    const output = document.querySelector('#output');
    let stop = () => {};
    let generation = 0;
    document.querySelector('#connect').addEventListener('click', async () => {
        const current = ++generation;
        stop();
        output.textContent = 'Discovering';
        const presented = token.value;
        const selected = session.value;
        try {
            const sessions = await discover(base, presented);
            if (current !== generation) return;
            if (!selected) {
                output.textContent = sessions.filter(item => item.kind !== 'headless').map(item => item.session_id).join('\n');
                return;
            }
            stop = observe(base, presented, selected, value => {
                output.textContent = `${value.status}${value.incomplete ? ' (incomplete retained preview)' : ''}\n${value.lines.join('\n')}`;
            }, socket);
        } catch { if (current !== generation) return; output.textContent = 'Discovery failed; verify origin and token'; }
    });
    return () => { ++generation; stop(); };
}

if (typeof document !== 'undefined') mountViewer(document);
