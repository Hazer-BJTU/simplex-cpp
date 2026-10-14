/** Minimal terminal observer: owns transport choice and text presentation. */
import { pathToFileURL } from 'node:url';
import { discover, observe } from '../observer.mjs';

/** Print ordinary session IDs, or follow a specified session until disconnected. */
export async function runTerminal(base, session, token, write = text => process.stdout.write(text), socket) {
    const sessions = await discover(base, token);
    if (!session) {
        write(sessions.filter(item => item.kind !== 'headless').map(item => item.session_id).join('\n') + '\n');
        return () => {};
    }
    return observe(base, token, session, value => {
        write(`${value.status}${value.incomplete ? ' (retained/preview history is incomplete)' : ''}\n`);
        write(value.lines.slice(-5).join('\n') + '\n');
    }, socket);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
    const [base, session] = process.argv.slice(2);
    if (!base) { console.error('Usage: node tui/main.mjs HUB_ORIGIN [SESSION_ID]'); process.exitCode = 1; }
    else {
        try { await runTerminal(base, session, process.env.SIMPLEX_PANEL_TOKEN ?? ''); }
        catch { console.error('Cannot discover Hub sessions; verify origin and operator token.'); process.exitCode = 1; }
    }
}
