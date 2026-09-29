/** Plain-text source editor with aligned line numbers and syntax highlighting.
 * Source stays authoritative; highlighting never rewrites user input. */
import { useMemo } from 'react';
import hljs from 'highlight.js/lib/core';
import json from 'highlight.js/lib/languages/json';
import yaml from 'highlight.js/lib/languages/yaml';

hljs.registerLanguage('json', json);
hljs.registerLanguage('yaml', yaml);

export function ConfigEditor({ text, language, onChange, disabled }: {
    text: string;
    language: 'json' | 'yaml';
    onChange: (text: string) => void;
    disabled: boolean;
}) {
    const lines = text.split('\n');
    const highlighted = useMemo(() => hljs.highlight(text + '\n', { language }).value, [text, language]);
    const width = lines.reduce((longest, line) => Math.max(longest, line.length), 60) + 4;
    return (
        <div className="h-[45dvh] min-h-48 overflow-auto rounded-md border border-line bg-surface" data-testid="config-editor">
            <div className="flex min-h-full font-mono text-xs leading-5" style={{ minWidth: `${width + 6}ch` }}>
                <pre aria-hidden="true" className="sticky left-0 z-10 m-0 shrink-0 select-none border-r border-line bg-sunken px-2 py-3 text-right text-ink-faint">
                    {lines.map((_, index) => index + 1).join('\n')}
                </pre>
                <div className="relative min-w-0 flex-1">
                    <pre aria-hidden="true" className="pointer-events-none m-0 whitespace-pre p-3 font-mono text-xs leading-5 text-ink"
                        dangerouslySetInnerHTML={{ __html: highlighted }} />
                    <textarea aria-label="Configuration source" value={text} disabled={disabled}
                        wrap="off" spellCheck={false} autoCapitalize="off" autoCorrect="off"
                        onChange={event => onChange(event.target.value)}
                        className="absolute inset-0 m-0 h-full w-full resize-none overflow-hidden border-0 bg-transparent p-3 font-mono text-xs leading-5 text-transparent caret-ink focus:outline-none"
                        style={{ tabSize: 4 }} />
                </div>
            </div>
        </div>
    );
}
