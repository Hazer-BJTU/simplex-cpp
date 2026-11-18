/** Read-only skill library. Source files are operator-owned, bounded UTF-8 YAML. */
import { constants } from 'node:fs';
import { mkdir, lstat, open, readdir, readFile, writeFile, link, rm } from 'node:fs/promises';
import { join } from 'node:path';
import { createHash, randomUUID } from 'node:crypto';
import { parseDocument } from 'yaml';
import { hubRoot } from '../config.ts';
import { configError } from '../configurations/store.ts';
import type { SkillDocument, SkillList } from '../../ui/skills.ts';

const MAX_BYTES = 64 * 1024;
const ID = /^[A-Za-z0-9_-]{1,128}$/;
const PACKAGED = join(hubRoot, 'schemas/skills');

/** Filename stems in a skill directory, excluding the reserved template id. */
function skillIds(entries: string[]): string[] {
    return entries.filter(n => n.endsWith('.yaml') && ID.test(n.slice(0, -5)))
        .map(n => n.slice(0, -5)).filter(id => id !== 'template').sort();
}

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

/** Best-effort packaged seeding must not hide an otherwise readable library. */
function failureMessage(error: unknown): string {
    return error instanceof Error ? error.message : String(error);
}

/** No request can select a file outside this library or follow a file symlink. */
export class SkillStore {
    readonly directory: string;
    private readonly packaged: string;
    private initialization: Promise<boolean> | undefined;
    private seedFailures = new Map<string, string>();

    constructor(dataDir: string, packagedDirectory = PACKAGED) {
        this.directory = join(dataDir, 'skills');
        this.packaged = packagedDirectory;
    }

    async template(): Promise<string> {
        return readFile(join(this.packaged, 'code-review.yaml'), 'utf8');
    }

    /** Initialize lazily so malformed local skill files never prevent Hub startup. */
    private async prepare(): Promise<void> {
        await mkdir(this.directory, { recursive: true, mode: 0o700 });
        const stat = await lstat(this.directory);
        if (!stat.isDirectory() || stat.isSymbolicLink()) {
            throw configError('Skill directory must be a regular directory');
        }
        // Concurrent reads share one attempt. A failed seed is not cached: the
        // next read retries it after the missing directory or permission returns.
        if (!this.initialization) {
            const attempt = this.seedPackaged();
            this.initialization = attempt;
            void attempt.then((complete) => {
                if (!complete && this.initialization === attempt) this.initialization = undefined;
            });
        }
        await this.initialization;
    }

    /** Write validated bytes to a fresh temporary file; tests may fail mid-write. */
    protected async writeTemporary(path: string, bytes: Uint8Array): Promise<void> {
        await writeFile(path, bytes, { flag: 'wx', mode: 0o600 });
    }

    /** Seed packaged examples without replacing operator-owned files.
     * A failed example is reported by list() and must not hide readable files. */
    private async seedPackaged(): Promise<boolean> {
        const failures = new Map<string, string>();
        try {
            for (const id of skillIds(await readdir(this.packaged))) {
                try {
                    const bytes = await readFile(join(this.packaged, `${id}.yaml`));
                    parseSkill(id, bytes); // A corrupt installation must not seed unusable files.
                    const destination = join(this.directory, `${id}.yaml`);
                    const temporary = join(this.directory, `.${id}-${randomUUID()}.tmp`);
                    try {
                        await this.writeTemporary(temporary, bytes);
                        try {
                            // Atomic create-if-absent: an existing destination is
                            // operator-owned and must keep its exact contents.
                            await link(temporary, destination);
                        } catch (error) {
                            if ((error as NodeJS.ErrnoException).code !== 'EEXIST') throw error;
                        }
                    } finally {
                        // A failed or partial temporary write is never visible as
                        // a skill; the next read retries the whole seed.
                        await rm(temporary, { force: true }).catch(() => {});
                    }
                } catch (error) {
                    failures.set(id, `packaged example: ${failureMessage(error)}`);
                }
            }
        } catch (error) {
            failures.set('packaged examples', failureMessage(error));
        }
        this.seedFailures = failures;
        return failures.size === 0;
    }

    async read(id: string): Promise<SkillDocument> {
        if (!ID.test(id) || id === 'template') throw configError('Invalid skill ID');
        await this.prepare();
        return this.readPrepared(id);
    }

    /** Read a validated id after prepare(); list() shares one seeding attempt. */
    private async readPrepared(id: string): Promise<SkillDocument> {
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
        const ids = skillIds(await readdir(this.directory));
        if (ids.length > 256) throw configError('Skill library exceeds 256 files');
        for (const id of ids) {
            try {
                const { text: _text, ...summary } = await this.readPrepared(id);
                result.skills.push(summary);
            } catch (error) {
                result.errors.push({ id, message: error instanceof Error ? error.message : 'Invalid skill' });
            }
        }
        // A failed packaged example is a diagnostic, not a library-wide failure.
        // Do not repeat it when the same id is already readable or already reported.
        const available = new Set(result.skills.map(skill => skill.id));
        const reported = new Set(result.errors.map(error => error.id));
        for (const [id, message] of this.seedFailures) {
            if (available.has(id) || reported.has(id)) continue;
            result.errors.push({ id, message });
        }
        return result;
    }
}
