/**
 * @file the panel shell.
 *
 * The old panel was a fixed 260px / 1fr / 320px grid with `overflow: hidden`,
 * an always-open inspector, and ten peer buttons in the header. This stage
 * replaces the frame and the way state reaches it; the transcript's own
 * presentation, the inspector drawer, the command palette and the design system
 * come next.
 *
 * What is already load-bearing here: the shell renders from selectors, so a
 * transcript event never re-renders the session list, and the window is a
 * column of full-height rows rather than a grid with an implicit row that
 * silently swallowed the composer.
 */
import { Approvals } from './Approvals.tsx';
import { Composer } from './Composer.tsx';
import { SessionHeader } from './SessionHeader.tsx';
import { SessionList } from './SessionList.tsx';
import { StatusBar } from './StatusBar.tsx';
import { Transcript } from './Transcript.tsx';
import { usePanel } from '../state/usePanel.ts';

export function App() {
    const workerProtocol = usePanel((state) => state.hub?.worker_protocol ?? '');

    return (
        <div className="flex h-full flex-col bg-slate-50 text-slate-900">
            <StatusBar />
            <div className="flex min-h-0 flex-1">
                <SessionList />
                <main className="flex min-w-0 flex-1 flex-col bg-white">
                    <SessionHeader />
                    <Approvals />
                    <Transcript />
                    <Composer />
                </main>
            </div>
            <footer className="flex items-center gap-2 border-t border-slate-200 bg-white px-4 py-1
                text-[11px] text-slate-400">
                <span className="font-mono">
                    worker protocol {workerProtocol || 'unknown'}
                </span>
                <span>
                    · a response appears when it is complete: the worker protocol carries whole
                    messages, not token deltas
                </span>
            </footer>
        </div>
    );
}
