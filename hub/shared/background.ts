/** Validated startup appearance settings; no filesystem or browser dependencies. */
export interface BackgroundOptions {
    /** Hub-local image path; empty disables decoration. */
    image: string;
    /** Restricted CSS background-size value. */
    size: string;
    /** One of the nine named background positions. */
    position: string;
    /** Image opacity in each resolved theme. */
    opacity: { light: number; dark: number };
    /** Tool card surface opacity in each theme; zero lets the image through. */
    toolCardOpacity: { light: number; dark: number };
}

const positions = ['center', 'top', 'bottom', 'left', 'right',
    'top left', 'top right', 'bottom left', 'bottom right'];

function mapping(value: unknown): value is Record<string, unknown> {
    return value !== null && typeof value === 'object' && !Array.isArray(value);
}

/** Shared numeric-or-per-theme opacity grammar; both fields use the same range. */
function opacityPair(value: unknown, field: string, fallback: { light: number; dark: number }):
    { light: number; dark: number } {
    const raw = value ?? fallback;
    const pair = typeof raw === 'number' ? { light: raw, dark: raw } : raw;
    if (!mapping(pair) || Object.keys(pair).some(key => !['light', 'dark'].includes(key))
        || !['light', 'dark'].every(key => typeof pair[key] === 'number'
            && Number.isFinite(pair[key]) && (pair[key] as number) >= 0 && (pair[key] as number) <= 1)) {
        throw new Error(`panel.background: ${field} must be a number in [0, 1] or { light, dark } in that range`);
    }
    return { light: pair.light as number, dark: pair.dark as number };
}

/** Reject arbitrary CSS and normalize optional fields, including numeric opacity shorthand. */
export function normalizeBackground(value: unknown): BackgroundOptions {
    const fail = (message: string): never => { throw new Error(`panel.background: ${message}`); };
    if (!mapping(value)) return fail('must be an object');
    for (const key of Object.keys(value)) {
        if (!['image', 'size', 'position', 'opacity', 'toolCardOpacity'].includes(key)) fail(`unknown field ${key}`);
    }
    const image = value.image ?? '';
    if (typeof image !== 'string' || image.includes('\0') || /^[a-z]+:\/\//i.test(image)
        || /^(data|file|blob):/i.test(image) || image.includes('${')) return fail('image must be a local file path');
    const size = value.size ?? 'cover';
    if (typeof size !== 'string' || size.length > 64) return fail('invalid size');
    if (!['cover', 'contain', 'auto'].includes(size)) {
        const parts = size.split(' ');
        if (parts.length < 1 || parts.length > 2 || !parts.every(part => part === 'auto'
            || (/^(?:\d+(?:\.\d+)?|\.\d+)(?:px|%)$/.test(part)
                && Number.parseFloat(part) > 0 && Number.isFinite(Number.parseFloat(part))))) {
            return fail('size must be cover, contain, auto, or one/two positive px/% dimensions');
        }
    }
    const position = value.position ?? 'center';
    if (typeof position !== 'string' || !positions.includes(position)) return fail('invalid position');
    return {
        image,
        size,
        position,
        opacity: opacityPair(value.opacity, 'opacity', { light: 0.12, dark: 0.08 }),
        toolCardOpacity: opacityPair(value.toolCardOpacity, 'toolCardOpacity', { light: 0.6, dark: 0.6 }),
    };
}
