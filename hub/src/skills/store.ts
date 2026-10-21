/** Read-only skill library. Source files are operator-owned, bounded UTF-8 YAML. */
import { constants } from 'node:fs';
import { mkdir, lstat, open, readdir, readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
import { parseDocument } from 'yaml';
import { hubRoot } from '../config.ts';
import { configError } from '../configurations/store.ts';
import type { SkillDocument, SkillList } from '../../ui/skills.ts';

const MAX_BYTES = 64 * 1024;
const ID = /^[A-Za-z0-9_-]{1,128}$/;

/** One source file, parsed without executable tags, aliases or includes. */
export function parseSkill(id: string, bytes: Uint8Array): SkillDocument {
    if (bytes.byteLength > MAX_BYTES) throw configError('Skill exceeds 64 KiB');
    let text: string;
    try { text = new TextDecoder('utf-8', { fatal: true }).decode(bytes); }
    catch { throw configError('Skill must be UTF-8'); }
    if (text.includes('\0')) throw configError('Skill must not contain NUL');
    const document = parseDocument(text, { uniqueKeys: true });
    if (document.errors.length) throw configError('Invalid skill YAML');
    let value: unknown;
    try { value = document.toJS({ maxAliasCount: 0 }); }
    catch { throw configError('Skill aliases are not supported'); }
    if (!value || typeof value !== 'object' || Array.isArray(value)) {
        throw configError('Skill must be a YAML object');
    }
    const data = value as Record<string, unknown>;
    for (const key of Object.keys(data)) {
        if (!['name', 'title', 'description', 'keywords', 'text'].includes(key)) {
            throw configError('Unknown skill field');
        }
    }
    function field(key: string, maximum: number): string {
        const v = data[key];
        if (typeof v !== 'string' || !v.trim() || v.includes('\0') || v.length > maximum) {
            throw configError(`Invalid skill ${key}`);
        }
        if (key !== 'text' && /[\r\n]/.test(v)) throw configError(`Invalid skill ${key}`);
        return v;
    }
    const keywords = data.keywords ?? [];
    if (!Array.isArray(keywords) || keywords.length > 32
        || keywords.some(v => typeof v !== 'string' || !v.trim() || v.length > 128 || /[\r\n\0]/.test(v))) {
        throw configError('Invalid skill keywords');
    }
    return { id, name: field('name', 128), title: field('title', 128),
        description: field('description', 512), keywords,
        text: field('text', MAX_BYTES),
        revision: createHash('sha256').update(bytes).digest('hex') };
}

/** No request can select a file outside this library or follow a file symlink. */
export class SkillStore {
    readonly directory: string;
    private initialization: Promise<void> | undefined;
    constructor(dataDir: string) { this.directory = join(dataDir, 'skills'); }

    async template(): Promise<string> {
        return readFile(join(hubRoot, 'schemas/skills/code-review.yaml'), 'utf8');
    }

    /** Initialize lazily so malformed local skill files never prevent Hub startup. */
    private async prepare(): Promise<void> {
        await mkdir(this.directory, { recursive: true, mode: 0o700 });
        const stat = await lstat(this.directory);
        if (!stat.isDirectory() || stat.isSymbolicLink()) {
            throw configError('Skill directory must be a regular directory');
        }
        // Concurrent picker/list reads share publication of the example file.
        this.initialization ??= this.seedExample();
        await this.initialization;
    }

    private async seedExample(): Promise<void> {
        try {
            await writeFile(join(this.directory, 'code-review.yaml'), await this.template(),
                { flag: 'wx', mode: 0o600 });
        } catch (error) {
            if ((error as NodeJS.ErrnoException).code !== 'EEXIST') throw error;
        }
    }

    async read(id: string): Promise<SkillDocument> {
        if (!ID.test(id) || id === 'template') throw configError('Invalid skill ID');
        await this.prepare();
        let file;
        try {
            const entry = await lstat(join(this.directory, `${id}.yaml`));
            if (!entry.isFile() || entry.isSymbolicLink()) throw configError('Skill must be a regular file');
            file = await open(join(this.directory, `${id}.yaml`),
                constants.O_RDONLY | constants.O_NOFOLLOW | constants.O_NONBLOCK);
        } catch (error) {
            throw configError((error as NodeJS.ErrnoException).code === 'ENOENT'
                ? 'Skill not found' : 'Skill must be a readable regular file',
            (error as NodeJS.ErrnoException).code === 'ENOENT' ? 404 : 400);
        }
        try {
            const stat = await file.stat();
            if (!stat.isFile()) throw configError('Skill must be a regular file');
            const buffer = Buffer.alloc(MAX_BYTES + 1);
            let used = 0;
            while (used < buffer.length) {
                const { bytesRead } = await file.read(buffer, used, buffer.length - used, null);
                if (!bytesRead) break;
                used += bytesRead;
            }
            return parseSkill(id, buffer.subarray(0, used));
        } finally { await file.close(); }
    }

    async list(): Promise<SkillList> {
        await this.prepare();
        const result: SkillList = { skills: [], errors: [] };
        // Bound work and response size even for a mistakenly populated directory.
        const entries = await readdir(this.directory);
        const ids = entries.filter(n => n.endsWith('.yaml') && ID.test(n.slice(0, -5)))
            .map(n => n.slice(0, -5)).filter(id => id !== 'template').sort();
        if (ids.length > 256) throw configError('Skill library exceeds 256 files');
        for (const id of ids) {
            try {
                const { text: _text, ...summary } = await this.read(id);
                result.skills.push(summary);
            } catch (error) {
                result.errors.push({ id, message: error instanceof Error ? error.message : 'Invalid skill' });
            }
        }
        return result;
    }
}
