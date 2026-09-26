/**
 * @file markdown rendering, without ever writing markup as HTML.
 *
 * `react-markdown` builds a React element tree from the AST; it does not go
 * through `innerHTML`, and without `rehype-raw` it does not parse inline HTML
 * either. So "render the model's markdown" and "never write panel markup as
 * HTML" — the contract `test/panel-assets.test.js` has enforced since long
 * before this rewrite — are not in tension. The contract's job changes from
 * "no markup anywhere" to "no `dangerouslySetInnerHTML`", which is the shape
 * the security test now checks for.
 *
 * Model output is untrusted text that happens to be markdown. Everything here
 * keeps it that way:
 *
 * - no raw HTML plugin, so `<img onerror=…>` in a response is text;
 * - links are built only for `http(s)`, and carry `rel="noreferrer noopener"`;
 * - highlight.js is applied through `rehype-highlight`, which produces
 *   elements rather than an HTML string.
 */
import {
    Children,
    isValidElement,
    useRef,
    useState,
    type ComponentPropsWithoutRef,
    type ReactNode,
} from 'react';
import ReactMarkdown, { type Components, type ExtraProps } from 'react-markdown';
import remarkGfm from 'remark-gfm';
import rehypeHighlight from 'rehype-highlight';
import { fenceFor } from './content.ts';

/** Copy text to the clipboard, reporting whether it worked. */
async function copyText(text: string): Promise<boolean> {
    try {
        await navigator.clipboard.writeText(text);
        return true;
    } catch {
        // A blocked clipboard is not an error worth a dialog; the button simply
        // does not claim to have copied anything.
        return false;
    }
}

/** A copy control that reports what actually happened. */
export function CopyButton({ text, label }: { text: () => string; label: string }) {
    const [state, setState] = useState<'idle' | 'done' | 'failed'>('idle');
    return (
        <button
            type="button"
            className="rounded px-1.5 py-0.5 text-[11px] text-slate-500 hover:bg-slate-200
                hover:text-slate-800"
            onClick={() => {
                void copyText(text()).then((ok) => {
                    setState(ok ? 'done' : 'failed');
                    setTimeout(() => setState('idle'), 1500);
                });
            }}
        >
            {state === 'done' ? 'copied' : state === 'failed' ? 'clipboard blocked' : label}
        </button>
    );
}

/**
 * The `<pre>` renderer: a header with the language and a copy button.
 *
 * The text is read back off the DOM node rather than tracked separately, so
 * what is copied is exactly what is displayed — including the highlighting
 * spans the highlighter inserted.
 */
function CodeBlock({ children, node: _node, ...rest }: {
    children?: ReactNode;
} & ComponentPropsWithoutRef<'pre'> & ExtraProps) {
    const ref = useRef<HTMLPreElement>(null);
    const first = Children.toArray(children)[0];
    const className = isValidElement<{ className?: string }>(first)
        ? first.props.className ?? ''
        : '';
    const language = /language-([\w+#.-]+)/.exec(className)?.[1] ?? '';
    // A block with no language that still came back highlighted was guessed at,
    // and saying so is more honest than labelling it "text".
    const label = language || (/\bhljs\b/.test(className) ? 'auto-detected' : 'text');

    return (
        <div className="group relative my-2 overflow-hidden rounded border border-slate-200">
            <div className="flex items-center gap-2 border-b border-slate-200 bg-slate-50 px-2 py-0.5">
                <span className="font-mono text-[11px] text-slate-500">{label}</span>
                <span className="flex-1" />
                <CopyButton label="copy" text={() => ref.current?.textContent ?? ''} />
            </div>
            <pre ref={ref} {...rest} className="overflow-x-auto bg-slate-50/50 p-2 text-[12px]">
                {children}
            </pre>
        </div>
    );
}

/**
 * The `<a>` renderer.
 *
 * Only `http(s)` becomes a link. Anything else — `javascript:`, `data:`, a
 * bare reference the model invented — is shown as text with its target beside
 * it, because a protocol-relative URL in model output is data, not a
 * navigation the operator asked for.
 */
function Link({ href, children, node: _node, ...rest }: {
    href?: string | undefined;
    children?: ReactNode;
} & ComponentPropsWithoutRef<'a'> & ExtraProps) {
    if (!href || !/^https?:\/\//i.test(href)) {
        return (
            <span className="text-slate-700">
                {children}
                {href && <span className="ml-1 text-[11px] text-slate-400">({href} — not opened)</span>}
            </span>
        );
    }
    return (
        <a
            {...rest}
            href={href}
            target="_blank"
            rel="noreferrer noopener"
            className="text-sky-700 underline underline-offset-2 hover:text-sky-900"
        >
            {children}
        </a>
    );
}

const COMPONENTS: Components = { pre: CodeBlock, a: Link };

/**
 * Render one markdown document.
 *
 * `remark-gfm` adds tables, task lists, strikethrough and autolinks;
 * `rehype-highlight` adds the language classes the stylesheet colours. It
 * registers highlight.js's *common* set rather than all of it, and an unknown
 * fence language is reported by the plugin rather than thrown — the difference
 * between a strange-looking code block and no transcript at all.
 *
 * The whole stack is about 50 kB gzipped, which is most of what this stage adds
 * to the bundle. That is a deliberate trade for a tool that runs on loopback:
 * the panel is not downloaded over a network anyone is paying for.
 */
export function Markdown({ children }: { children: string }) {
    return (
        <div className="md">
            <ReactMarkdown
                remarkPlugins={[remarkGfm]}
                rehypePlugins={[[rehypeHighlight, { detect: true }]]}
                components={COMPONENTS}
            >
                {children}
            </ReactMarkdown>
        </div>
    );
}

/**
 * Render a short string through the same pipeline, for a fenced block.
 *
 * Used for a command argument, so a shell command gets the same highlighting
 * and the same copy button as a code block in a response — one code path, not
 * two. The fence is grown past any run of backticks in the content, because a
 * command containing ``` would otherwise close the fence early and the rest
 * would be parsed as markdown.
 */
export function MarkdownBlock({ code, language }: { code: string; language: string }) {
    const fence = fenceFor(code);
    return <Markdown>{`${fence}${language}\n${code}\n${fence}`}</Markdown>;
}
