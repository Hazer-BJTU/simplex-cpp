/**
 * @file one tool call, and what came back.
 *
 * The old panel rendered a call as `JSON.stringify(arguments, null, 2)` and a
 * result as raw text, which is why a `run_command` read as
 * `{"command": "…"}` and its output as a wall of punctuation. Four things
 * change that here:
 *
 * - **A command reads as a command.** When the arguments carry a `command`
 *   string it is shown as a highlighted shell line with the rest of the
 *   arguments beside it, rather than as JSON with the command buried inside.
 * - **The result is read for structure.** The process tools render
 *   `[[field]]: value` lines and named output blocks; those become a field list
 *   and labelled stdout/stderr sections. The parser is a display heuristic and
 *   says so — anything it does not recognise is shown verbatim.
 * - **Status is a state, not a colour.** Pending approval, running, ok, failed,
 *   skipped and "the run ended without a result" are five different things, and
 *   the card spells out which one it is.
 * - **Details are deliberate.** A short call summary is always visible; the
 *   complete arguments, output, and timing appear only when opened.
 */
import { useState } from 'react';
import { Glyph, type GlyphName } from '../ui/icons.tsx';
import { MarkdownBlock } from './Markdown.tsx';
import { formatDuration, prettyJson } from './content.ts';
import type { ToolCall, CallStatus } from './rounds.ts';
import type { OutputBlock, OutputDocument } from './toolOutput.ts';

/** How long a result may be before it is folded away. */
const COLLAPSED_CHARS = 1200;
const SUMMARY_CHARS = 160;

/** A bounded, single-line hint; full values remain available in the details. */
function callSummary(args: unknown): string {
    if (typeof args !== 'object' || args === null || Array.isArray(args)) return '';
    const record = args as Record<string, unknown>;
    const key = ['command', 'path', 'file_path', 'url', 'query']
        .find((candidate) => typeof record[candidate] === 'string');
    if (key) {
        const value = String(record[key]);
        const firstLine = value.split(/\r?\n/, 1)[0] ?? '';
        return firstLine.length > SUMMARY_CHARS || firstLine.length < value.length
            ? `${firstLine.slice(0, SUMMARY_CHARS)}…`
            : firstLine;
    }
    return Object.keys(record).join(', ');
}

/**
 * How a status reads, how it is coloured, and which glyph it carries.
 *
 * The glyph is not decoration: the six states were six tinted chips, and a
 * reader who cannot separate the amber from the green had only the words — but
 * the words are the same shape at a glance ("ok", "not run", "no result
 * reported"). A shape distinguishes them before the word is read.
 */
const STATUS: Record<CallStatus, { label: string; tone: string; icon: GlyphName }> = {
    pending: {
        label: 'waiting for approval',
        tone: 'bg-warn-soft text-warn ring-warn-line',
        icon: 'approval',
    },
    running: { label: 'running', tone: 'bg-info-soft text-info ring-info-line', icon: 'spinner' },
    ok: { label: 'ok', tone: 'bg-ok-soft text-ok ring-ok-line', icon: 'ok' },
    failed: { label: 'failed', tone: 'bg-danger-soft text-danger ring-danger-line', icon: 'error' },
    skipped: { label: 'not run', tone: 'bg-subtle text-ink-muted ring-line', icon: 'cancel' },
    unknown: {
        label: 'no result reported',
        tone: 'bg-subtle text-ink-muted ring-line',
        icon: 'warning',
    },
};

/** The edge a card carries, so a status survives being scrolled past. */
const BORDER: Record<CallStatus, string> = {
    pending: 'border-l-warn-line',
    running: 'border-l-info-line',
    ok: 'border-l-ok-line',
    failed: 'border-l-danger-line',
    skipped: 'border-l-line-strong',
    unknown: 'border-l-line-strong',
};

/**
 * Split arguments into a command line and everything else.
 *
 * Shape-driven rather than name-driven: any tool whose arguments carry a
 * `command` string is showing a command, whatever it is called. A tool that
 * does not stays JSON, which is honest — inventing a rendering for an argument
 * this panel does not understand is how a card starts lying.
 */
export function splitCommand(args: unknown): { command: string; rest: unknown } | null {
    if (typeof args !== 'object' || args === null || Array.isArray(args)) return null;
    const record = args as Record<string, unknown>;
    if (typeof record.command !== 'string' || record.command.length === 0) return null;
    const rest: Record<string, unknown> = {};
    for (const [key, value] of Object.entries(record)) {
        if (key !== 'command') rest[key] = value;
    }
    return { command: record.command, rest: Object.keys(rest).length > 0 ? rest : null };
}

/** One labelled output section. */
function Block({ block }: { block: OutputBlock }) {
    const [open, setOpen] = useState(block.text.length <= COLLAPSED_CHARS);
    const isError = /^stderr/i.test(block.name);
    const long = block.text.length > COLLAPSED_CHARS;
    return (
        <div className={`rounded border-l-2 bg-sunken ${isError ? 'border-danger-line' : 'border-line-strong'}`}>
            <div className="flex items-center gap-2 px-2 py-0.5">
                <span className="font-mono text-xs font-medium text-ink-muted">{block.name}</span>
                <span className="text-xs text-ink-faint">
                    {block.empty
                        ? 'empty'
                        : `${block.bytes ?? block.text.length} bytes${block.truncated ? ' (truncated by the tool)' : ''}`}
                </span>
                <span className="flex-1" />
                {long && (
                    <button
                        type="button"
                        className="rounded px-1.5 py-0.5 text-xs text-ink-muted hover:bg-line"
                        onClick={() => setOpen((value) => !value)}
                    >
                        {open ? 'collapse' : 'expand all'}
                    </button>
                )}
            </div>
            {open && block.text && (
                <pre className="max-h-96 overflow-auto whitespace-pre-wrap break-words px-2 pb-1.5
                    font-mono text-xs text-ink">
                    {block.text}
                </pre>
            )}
        </div>
    );
}

/** The fields the tool reported about itself. */
function Fields({ fields }: { fields: OutputDocument['fields'] }) {
    if (fields.length === 0) return null;
    return (
        <dl className="flex flex-wrap gap-x-3 gap-y-0.5 px-2 py-1 text-xs">
            {fields.map((field) => (
                <div key={field.name} className="flex gap-1">
                    <dt className="text-ink-faint">{field.name}</dt>
                    <dd className="font-mono text-ink">{field.value}</dd>
                </div>
            ))}
        </dl>
    );
}

/** A result, however it turned out to be shaped. */
function Result({ call }: { call: ToolCall }) {
    const [raw, setRaw] = useState(false);
    const result = call.result;
    if (!result) return null;

    return (
        <div className="mt-1 space-y-1">
            {call.status === 'skipped' && (
                <p className="px-2 text-xs text-ink-muted">
                    the loop did not dispatch this call — not an execution, and not a failure
                </p>
            )}

            {result.text === '' && (
                <p className="px-2 text-xs italic text-ink-faint">(no output content)</p>
            )}

            {call.output?.kind === 'document' && !raw && (
                <div className="space-y-1">
                    <Fields fields={call.output.document.fields} />
                    {call.output.document.blocks.map((block) => (
                        <Block key={block.name} block={block} />
                    ))}
                    {call.output.document.rest.length > 0 && (
                        <pre className="max-h-96 overflow-auto whitespace-pre-wrap break-words rounded
                            bg-sunken px-2 py-1 font-mono text-xs text-ink">
                            {call.output.document.rest.join('\n')}
                        </pre>
                    )}
                </div>
            )}

            {call.output?.kind === 'text' && !raw && result.text !== '' && (
                <pre className="max-h-96 overflow-auto whitespace-pre-wrap break-words rounded bg-sunken
                    px-2 py-1 font-mono text-xs text-ink">
                    {result.text}
                </pre>
            )}

            {raw && (
                <pre className="max-h-96 overflow-auto whitespace-pre-wrap break-words rounded bg-sunken
                    px-2 py-1 font-mono text-xs text-ink">
                    {result.text}
                </pre>
            )}

            <div className="flex items-center gap-2 px-2">
                {call.output?.kind === 'document' && (
                    <button
                        type="button"
                        className="text-xs text-ink-faint hover:text-ink"
                        onClick={() => setRaw((value) => !value)}
                    >
                        {raw ? 'as the tool rendered it' : 'raw output'}
                    </button>
                )}
                <span className="flex-1" />
                {call.reportedMs !== null && (
                    <span className="text-xs text-ink-muted">
                        the process reported {formatDuration(call.reportedMs)} of runtime
                    </span>
                )}
                {call.elapsedMs !== null && (
                    <span className="text-xs text-ink-faint">
                        {formatDuration(call.elapsedMs)} from proposal to result
                    </span>
                )}
            </div>
        </div>
    );
}

/** One proposed call, its status, and what came back. */
export function ToolCard({ call }: { call: ToolCall }) {
    const [showArgs, setShowArgs] = useState(false);
    const split = splitCommand(call.args);
    const status = STATUS[call.status];
    const hasArgs = call.args !== undefined && call.args !== null;
    const summary = callSummary(call.args);

    return (
        <article
            data-testid="tool-card"
            data-tool={call.name}
            data-status={call.status}
            className={`rounded border border-l-2 border-line bg-surface p-2 ${BORDER[call.status]}`}
        >
            <header className="flex flex-wrap items-center gap-2">
                <span className="font-mono text-sm font-medium text-ink">{call.name}</span>
                <span
                    data-testid="tool-status"
                    className={`inline-flex items-center gap-1 rounded-full px-2 py-0.5 text-xs
                        font-medium ring-1 ring-inset ${status.tone}`}
                >
                    <Glyph name={status.icon} size="sm" />
                    {status.label}
                </span>
            </header>

            {summary && (
                <p className="mt-1 truncate font-mono text-xs text-ink-muted" title={summary}>
                    {summary}
                </p>
            )}

            {call.status === 'pending' && call.prompt && (
                <p className="mt-1 rounded border border-warn-line bg-warn-soft px-2 py-1 text-xs text-warn">
                    this call is waiting for an approval — answer it above.
                </p>
            )}

            {call.status === 'failed' && call.result?.error && (
                <p className="mt-1 break-words text-xs text-danger">
                    {call.result.error.stage}
                    {call.result.error.message ? ` — ${call.result.error.message}` : ''}
                </p>
            )}

            <details data-testid="tool-details" className="mt-1 min-w-0 text-xs">
                <summary className="w-fit cursor-pointer select-none text-ink-muted hover:text-ink">
                    Call and result details
                </summary>
                <div className="mt-2 min-w-0 space-y-2 border-t border-line pt-2">
                    <div className="flex flex-wrap gap-x-3 gap-y-1 text-ink-faint">
                        {call.security && <span>security: {call.security}</span>}
                        {call.scheduling && <span>scheduling: {call.scheduling}</span>}
                        {call.id && <span className="font-mono">id: {call.id}</span>}
                    </div>
                    {split ? (
                        <div>
                            <MarkdownBlock code={split.command} language="bash" />
                            {split.rest !== null && <CallArguments value={split.rest} />}
                        </div>
                    ) : hasArgs && (
                        <pre className="max-h-96 overflow-auto whitespace-pre-wrap break-all rounded
                            bg-sunken px-2 py-1 font-mono text-ink">
                            {prettyJson(call.args)}
                        </pre>
                    )}
                    <Result call={call} />
                    <button
                        type="button"
                        className="text-ink-faint hover:text-ink"
                        onClick={() => setShowArgs((value) => !value)}
                    >
                        {showArgs ? 'hide raw call' : 'raw call'}
                    </button>
                    {showArgs && (
                        <pre className="max-h-96 overflow-auto whitespace-pre-wrap break-all rounded
                            bg-sunken px-2 py-1 font-mono text-ink">
                            {prettyJson(call.args ?? {})}
                        </pre>
                    )}
                </div>
            </details>
        </article>
    );
}

/** The arguments left over once a command has been taken out. */
function CallArguments({ value }: { value: unknown }) {
    const text = prettyJson(value);
    if (!text || text === '{}') return null;
    return (
        <details className="text-xs text-ink-muted">
            <summary className="cursor-pointer select-none">other arguments</summary>
            <pre className="mt-1 max-h-96 overflow-auto whitespace-pre-wrap break-all rounded bg-sunken
                px-2 py-1 font-mono text-ink">
                {text}
            </pre>
        </details>
    );
}
