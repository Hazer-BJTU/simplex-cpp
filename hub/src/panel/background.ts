/** Immutable, bounded decoration loaded once before the HTTP listeners start. */
import { constants } from 'node:fs';
import { open } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import type { IncomingMessage, ServerResponse } from 'node:http';
import { normalizeBackground } from '../../shared/background.ts';
import type { PanelBackground } from '../../ui/protocol.ts';
import type { Logger } from '../log.ts';

export const BACKGROUND_MAX_BYTES = 8 * 1024 * 1024;
const MAX_PIXELS = 32 * 1024 * 1024;

/** Retained bytes are private to this service, never included in metadata or state. */
export interface BackgroundAsset {
    metadata: PanelBackground;
    bytes: Buffer;
    contentType: 'image/png' | 'image/jpeg';
}

function dimensions(width: number, height: number): void {
    if (!width || !height || width * height > MAX_PIXELS) throw new Error('invalid or excessive image dimensions');
}

/** CRC of PNG chunk type + data; reject corrupt chunks before publishing bytes. */
function crc32(bytes: Buffer): number {
    let crc = 0xffffffff;
    for (const byte of bytes) {
        crc ^= byte;
        for (let i = 0; i < 8; i++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    return (crc ^ 0xffffffff) >>> 0;
}

/**
 * Validate container structure, dimensions and static format without a native
 * image dependency. Browser decode remains the final pixel-data check; failure
 * leaves the normal panel visible. Never infer a format from a filename alone.
 */
export function backgroundContentType(bytes: Buffer): BackgroundAsset['contentType'] {
    const invalid = (): never => { throw new Error('expected a valid static PNG or JPEG image'); };
    if (bytes.subarray(0, 8).equals(Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]))) {
        let offset = 8;
        let header = false;
        let data = false;
        while (offset + 12 <= bytes.length) {
            const length = bytes.readUInt32BE(offset);
            const end = offset + 12 + length;
            if (end > bytes.length) return invalid();
            const type = bytes.toString('ascii', offset + 4, offset + 8);
            if (crc32(bytes.subarray(offset + 4, end - 4)) !== bytes.readUInt32BE(end - 4)) return invalid();
            if (!header && type !== 'IHDR') return invalid();
            if (type === 'IHDR') {
                if (header || length !== 13) return invalid();
                dimensions(bytes.readUInt32BE(offset + 8), bytes.readUInt32BE(offset + 12));
                const depth = bytes[offset + 16];
                const color = bytes[offset + 17];
                const depths: Record<number, number[]> = { 0: [1, 2, 4, 8, 16], 2: [8, 16], 3: [1, 2, 4, 8], 4: [8, 16], 6: [8, 16] };
                if (!depths[color!]?.includes(depth!) || bytes[offset + 18] !== 0
                    || bytes[offset + 19] !== 0 || bytes[offset + 20]! > 1) return invalid();
                header = true;
            }
            if (['acTL', 'fcTL', 'fdAT'].includes(type)) throw new Error('animated PNG is not supported');
            if (type === 'IDAT') data = true;
            if (type === 'IEND') {
                if (length !== 0 || !data || end !== bytes.length) return invalid();
                return 'image/png';
            }
            offset = end;
        }
        return invalid();
    }
    if (bytes[0] === 0xff && bytes[1] === 0xd8) {
        let offset = 2;
        let frame = false;
        let scan = false;
        while (offset < bytes.length) {
            if (bytes[offset++] !== 0xff) return invalid();
            while (bytes[offset] === 0xff) offset++;
            const marker = bytes[offset++];
            if (marker === 0xd9) {
                if (!frame || !scan || offset !== bytes.length) return invalid();
                return 'image/jpeg';
            }
            if (marker === undefined || offset + 2 > bytes.length || marker === 0xd8 || marker === 0) return invalid();
            const length = bytes.readUInt16BE(offset);
            if (length < 2 || offset + length > bytes.length) return invalid();
            if ([0xc0, 0xc1, 0xc2].includes(marker)) {
                if (frame || length < 8) return invalid();
                dimensions(bytes.readUInt16BE(offset + 5), bytes.readUInt16BE(offset + 3));
                frame = true;
            }
            offset += length;
            if (marker === 0xda) {
                if (!frame) return invalid();
                scan = true;
                // Entropy-coded data escapes FF as FF00; restart markers are
                // standalone. Other markers return to ordinary segment parsing.
                while (offset < bytes.length) {
                    if (bytes[offset] !== 0xff) { offset++; continue; }
                    let next = offset + 1;
                    while (bytes[next] === 0xff) next++;
                    if (bytes[next] === 0 || (bytes[next]! >= 0xd0 && bytes[next]! <= 0xd7)) {
                        offset = next + 1;
                    } else break;
                }
            }
        }
    }
    return invalid();
}

/** File errors disable only the decoration. The configured pathname never reaches clients. */
export async function loadBackground(value: unknown, log: Logger): Promise<BackgroundAsset | null> {
    if (value === undefined) return null;
    const options = normalizeBackground(value);
    if (!options.image) return null;
    try {
        // NONBLOCK prevents an operator accidentally pointing at a FIFO from
        // hanging startup. Inspect the opened descriptor, not a separate path.
        const file = await open(options.image, constants.O_RDONLY | (constants.O_NONBLOCK ?? 0));
        let bytes: Buffer;
        try {
            const stat = await file.stat();
            if (!stat.isFile()) throw new Error('image must be a regular file');
            if (!stat.size || stat.size > BACKGROUND_MAX_BYTES) throw new Error('image exceeds the 8 MiB limit or is empty');
            const buffer = Buffer.alloc(BACKGROUND_MAX_BYTES + 1);
            let used = 0;
            while (used < buffer.length) {
                const { bytesRead } = await file.read(buffer, used, buffer.length - used, used);
                if (!bytesRead) break;
                used += bytesRead;
            }
            if (used > BACKGROUND_MAX_BYTES) throw new Error('image exceeds the 8 MiB limit');
            bytes = Buffer.from(buffer.subarray(0, used));
        } finally { await file.close(); }
        const contentType = backgroundContentType(bytes);
        const revision = createHash('sha256').update(bytes).digest('hex');
        return { bytes, contentType, metadata: { url: '/api/panel/background/image', revision,
            size: options.size, position: options.position, opacity: options.opacity,
            toolCardOpacity: options.toolCardOpacity } };
    } catch {
        log.warn('panel background disabled: use a readable static PNG/JPEG file (at most 8 MiB and 32 megapixels)');
        return null;
    }
}

/** Serve an already authorized request; no path/query can select another file. */
export function serveBackground(asset: BackgroundAsset | null, req: IncomingMessage, res: ServerResponse): void {
    res.setHeader('Cache-Control', 'private, no-cache');
    res.setHeader('Vary', 'Authorization');
    res.setHeader('X-Content-Type-Options', 'nosniff');
    if (!asset) { res.writeHead(404); res.end(); return; }
    const etag = `"${asset.metadata.revision}"`;
    res.setHeader('ETag', etag);
    if (req.headers['if-none-match']?.split(',').some(value => value.trim().replace(/^W\//, '') === etag || value.trim() === '*')) {
        res.writeHead(304); res.end(); return;
    }
    res.writeHead(200, { 'Content-Type': asset.contentType, 'Content-Length': asset.bytes.length });
    res.end(req.method === 'HEAD' ? undefined : asset.bytes);
}
