/**
 * @file the panel shell.
 *
 * The old panel was a fixed 260px / 1fr / 320px grid with `overflow: hidden`,
 * an inspector that was always open and whose panes accumulated, and ten peer
 * buttons in the header. The frame was not the problem; what it did with its
 * three columns was.
 *
 * The layout here is the design's: the session list, the conversation, and a
 * context drawer that is closed until it is asked for. Everything below the top
 * bar renders from selectors, so a transcript event does not re-render the
 * session list, and the palette and dialogs are mounted at the root because
 * they are reachable from anywhere — including from inside the composer, where
 * a keystroke otherwise belongs to the field.
 */
import { Approvals } from './Approvals.tsx';
import { CommandPalette } from './CommandPalette.tsx';
import { Composer } from './Composer.tsx';
import { Inspector } from './Inspector.tsx';
import { SessionHeader } from './SessionHeader.tsx';
import { SessionList } from './SessionList.tsx';
import { StatusBar } from './StatusBar.tsx';
import { Transcript } from './Transcript.tsx';
import { usePanel } from '../state/usePanel.ts';
import { TooltipProvider } from '../ui/overlays.tsx';

export function App() {
    const workerProtocol = usePanel((state) => state.hub?.worker_protocol ?? '');

    return (
        <TooltipProvider>
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
                    <Inspector />
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
                    <span className="flex-1" />
                    <span>⌘K for commands</span>
                </footer>
                <CommandPalette />
            </div>
        </TooltipProvider>
    );
}
