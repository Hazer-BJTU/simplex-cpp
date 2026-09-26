/**
 * @file the panel shell, as far as the scaffolding goes.
 *
 * This is deliberately not the transcript yet — that is the next stage. What it
 * does prove is the whole toolchain end to end: a React component with types, a
 * Tailwind stylesheet, and an import of `shared/protocol.ts` that the browser
 * bundle and the Node server resolve from the same file. If any link in that
 * chain is broken, this page does not build or does not render.
 */
import { CAPABILITIES, PANEL_PROTOCOL } from '../../../shared/protocol.ts';

export function App() {
    return (
        <main className="grid min-h-screen place-items-center bg-slate-50 p-6 text-slate-900">
            <section className="w-full max-w-2xl rounded-lg border border-slate-200 bg-white p-6 shadow-sm">
                <h1 className="text-xl font-semibold">simplex hub panel</h1>
                <p className="mt-1 text-sm text-slate-500">
                    Toolchain scaffolding. The transcript, composer and inspector
                    replace this at the next stage.
                </p>

                <dl className="mt-6 grid grid-cols-[10rem_1fr] gap-x-4 gap-y-2 text-sm">
                    <dt className="text-slate-500">protocol</dt>
                    <dd className="font-mono">
                        {PANEL_PROTOCOL.name} v{PANEL_PROTOCOL.version}
                    </dd>
                    <dt className="text-slate-500">capabilities</dt>
                    <dd className="flex flex-wrap gap-1">
                        {CAPABILITIES.map((capability) => (
                            <span
                                key={capability}
                                className="rounded border border-slate-300 px-1.5 font-mono
                                    text-xs text-slate-600"
                            >
                                {capability}
                            </span>
                        ))}
                    </dd>
                </dl>

                <p className="mt-6 border-t border-slate-200 pt-4 text-xs text-slate-500">
                    These values come from <code className="font-mono">shared/protocol.ts</code>,
                    the same module the hub reads — which is the point.
                </p>
            </section>
        </main>
    );
}
