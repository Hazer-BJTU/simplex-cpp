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
 *
 * Below `md` the two side columns stop being columns and become drawers over
 * the conversation, dismissed by their backdrop or by Escape. The old panel
 * kept the columns at every width, which on a 380px screen left the thing being
 * read squeezed between them.
 */
import { useEffect, useState } from 'react';
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
    // The session list is a drawer below `md`. On a wide screen this state is
    // simply irrelevant: the sidebar is visible either way.
    const [sessionsOpen, setSessionsOpen] = useState(false);

    // Escape closes the drawer. Radix does this for its own overlays; this one
    // is plain markup, so it needs the listener to behave like the rest.
    useEffect(() => {
        if (!sessionsOpen) return;
        function onKeyDown(event: KeyboardEvent): void {
            if (event.key === 'Escape') setSessionsOpen(false);
        }
        document.addEventListener('keydown', onKeyDown);
        return () => document.removeEventListener('keydown', onKeyDown);
    }, [sessionsOpen]);

    return (
        <TooltipProvider>
            <div className="flex h-full flex-col bg-app text-ink">
                <a
                    href="#conversation"
                    className="sr-only focus:not-sr-only focus:absolute focus:left-2 focus:top-2
                        focus:z-50 focus:rounded focus:bg-raised focus:px-3 focus:py-1
                        focus:text-xs focus:shadow-lg"
                >
                    skip to the conversation
                </a>
                <StatusBar onOpenSessions={() => setSessionsOpen(true)} />
                <div className="flex min-h-0 flex-1">
                    <SessionList open={sessionsOpen} onClose={() => setSessionsOpen(false)} />
                    <main
                        id="conversation"
                        className="flex min-w-0 flex-1 flex-col bg-surface focus:outline-none"
                    >
                        <SessionHeader />
                        <Approvals />
                        <Transcript />
                        <Composer />
                    </main>
                    <Inspector />
                </div>
                <footer
                    className="flex items-center gap-2 border-t border-line bg-surface px-4 py-1
                        text-xs text-ink-faint"
                >
                    <span className="shrink-0 font-mono">
                        worker protocol {workerProtocol || 'unknown'}
                    </span>
                    {/* The honest limitation, where it is always visible rather
                        than in a tooltip: there is no streaming here, and there
                        cannot be one without a worker-protocol change. */}
                    <span className="hidden truncate sm:inline">
                        · a response appears when it is complete: the worker protocol carries whole
                        messages, not token deltas
                    </span>
                    <span className="flex-1" />
                    <span className="hidden shrink-0 md:inline">⌘K for commands</span>
                </footer>
                <CommandPalette />
            </div>
        </TooltipProvider>
    );
}
