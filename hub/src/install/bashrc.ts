/** Optional PATH persistence is a separate transaction after worker installation. */
import { lstat, readFile, realpath } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { acquireLock, atomicWrite, exists } from './files.ts';

export const START = '# >>> simplex-hub managed worker PATH >>>';
export const END = '# <<< simplex-hub managed worker PATH <<<';

/** Literal Bash words, including quotes, dollar signs, backticks and newlines. */
export function bashQuote(value: string): string {
    return `'${value.replaceAll("'", "'\\''")}'`;
}
export function pathCommand(directory: string): string {
    return `export PATH=${bashQuote(join(directory, 'bin'))}:"$PATH"`;
}

/** Remove complete duplicates, replace at the first block, reject ambiguous markers. */
export function editBashrc(contents: string, directory: string): string {
    const newline = contents.includes('\r\n') ? '\r\n' : '\n';
    const lines = contents.match(/[^\n]*\n|[^\n]+$/g) ?? [];
    const output: string[] = [];
    const block = [START, pathCommand(directory), END].join(newline) + newline;
    let inside = false;
    let inserted = false;
    for (const raw of lines) {
        const line = raw.replace(/\r?\n$/, '');
        if (line === START) {
            if (inside) throw new Error('Malformed simplex-hub PATH markers in .bashrc');
            inside = true;
            if (!inserted) { output.push(block); inserted = true; }
        } else if (line === END) {
            if (!inside) throw new Error('Malformed simplex-hub PATH markers in .bashrc');
            inside = false;
        } else {
            if (line.includes(START) || line.includes(END)) throw new Error('Malformed simplex-hub PATH markers in .bashrc');
            if (!inside) output.push(raw);
        }
    }
    if (inside) throw new Error('Unterminated simplex-hub PATH block in .bashrc');
    let text = output.join('');
    if (!inserted) text += `${text && !text.endsWith('\n') ? newline : ''}${block}`;
    return text;
}

/**
 * Refuse symlinked .bashrc rather than silently replacing the link or modifying
 * a different file. A persistent per-home lock serializes installers targeting
 * different worker directories. Preserve existing permissions on atomic rewrite.
 */
export async function updateBashrc(home: string, directory: string): Promise<void> {
    const actualHome = await realpath(home);
    const path = join(actualHome, '.bashrc');
    const release = await acquireLock(join(dirname(path), '.simplex-hub-bashrc.lock'));
    try {
        let contents = '';
        let mode = 0o600;
        if (await exists(path)) {
            const stat = await lstat(path);
            if (!stat.isFile()) throw new Error('Refusing non-regular or symlinked .bashrc; update PATH manually');
            if (stat.size > 4 * 1024 * 1024) throw new Error('.bashrc is too large to update automatically');
            mode = stat.mode & 0o777;
            contents = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true }).decode(await readFile(path));
        }
        const edited = editBashrc(contents, directory);
        if (edited !== contents) await atomicWrite(path, edited, mode);
    } finally {
        await release();
    }
}
