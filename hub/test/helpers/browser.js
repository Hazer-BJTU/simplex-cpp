/**
 * @file test helper: a minimal Chrome DevTools Protocol client.
 *
 * The panel is the one part of the hub that no unit test can load — it is HTML,
 * CSS and ES modules with no build step, so only a browser can tell whether it
 * runs. This helper exists so that check costs one test file and no npm
 * dependency: it launches whatever Chrome the machine already has, speaks CDP
 * over the global WebSocket, and gets out of the way.
 *
 * Everything here is deliberately small. It is not a browser automation
 * library: `evaluate` and screenshot-free assertions are the whole surface.
 */
import { spawn } from 'node:child_process';
import { accessSync, constants, existsSync, mkdtempSync, readdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

/** Browser names looked up on PATH, in preference order. */
const PATH_CANDIDATES = [
    'google-chrome', 'google-chrome-stable', 'chromium', 'chromium-browser', 'chrome',
];

/** Locations of a cached headless shell, relative to the home directory. */
const CACHE_PATTERNS = [
    ['.cache', 'puppeteer', 'chrome-headless-shell'],
    ['.cache', 'puppeteer', 'chrome'],
];

/** True when the path is an executable file. */
function executable(path) {
    try {
        accessSync(path, constants.X_OK);
        return true;
    } catch {
        return false;
    }
}

/** First executable named `name` on PATH, or null. */
function onPath(name) {
    for (const directory of (process.env.PATH ?? '').split(':')) {
        if (!directory) continue;
        const candidate = join(directory, name);
        if (executable(candidate)) return candidate;
    }
    return null;
}

/** Cached browser binaries, newest version directory first. */
function cachedBrowsers() {
    const home = process.env.HOME;
    if (!home) return [];
    const found = [];
    for (const pattern of CACHE_PATTERNS) {
        const root = join(home, ...pattern);
        if (!existsSync(root)) continue;
        for (const version of readdirSync(root).sort().reverse()) {
            for (const relative of [
                ['chrome-headless-shell-linux64', 'chrome-headless-shell'],
                ['chrome-linux64', 'chrome'],
                ['chrome-linux', 'chrome'],
            ]) {
                const candidate = join(root, version, ...relative);
                if (executable(candidate)) found.push(candidate);
            }
        }
    }
    return found;
}

/** Locate a browser to drive, or null when the machine has none. */
export function findBrowser() {
    const configured = process.env.CHROME_BIN;
    if (configured && executable(configured)) return configured;
    for (const name of PATH_CANDIDATES) {
        const candidate = onPath(name);
        if (candidate) return candidate;
    }
    return cachedBrowsers()[0] ?? null;
}

/**
 * Whether a missing browser must fail instead of skipping.
 *
 * CI sets this: a browser check that silently skips is a check that silently
 * asserts nothing, and this suite exists precisely because nothing else looks at
 * the panel.
 */
export function browserRequired() {
    return process.env.PANEL_REQUIRE_BROWSER === '1';
}

/**
 * Launch a headless browser with remote debugging on a free port.
 *
 * @param {object} [options]
 * @param {string} [options.executable] browser path; defaults to findBrowser().
 * @param {number} [options.timeoutMs] how long to wait for the DevTools banner.
 * @returns {Promise<{process: object, port: number, stderr: () => string,
 *                    close: () => Promise<void>, release: () => void}>}
 */
export async function launchBrowser({ executable: binary, timeoutMs = 20000 } = {}) {
    const path = binary ?? findBrowser();
    if (!path) throw new Error('no browser found');
    const profile = mkdtempSync(join(tmpdir(), 'simplex-hub-chrome-'));
    const child = spawn(path, [
        '--headless',
        '--no-sandbox',
        '--disable-gpu',
        '--disable-dev-shm-usage',
        '--disable-extensions',
        '--disable-background-networking',
        '--no-first-run',
        '--no-default-browser-check',
        '--remote-debugging-port=0',
        `--user-data-dir=${profile}`,
        'about:blank',
    ], { stdio: ['ignore', 'pipe', 'pipe'] });

    let output = '';
    const banner = new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error(
            `no DevTools banner from ${path} within ${timeoutMs} ms:\n${output}`)), timeoutMs);
        timer.unref?.();
        const scan = (chunk) => {
            output += chunk;
            const match = /DevTools listening on (ws:\/\/\S+)/.exec(output);
            if (!match) return;
            clearTimeout(timer);
            resolve(match[1]);
        };
        child.stderr.on('data', (chunk) => scan(chunk.toString('utf8')));
        child.stdout.on('data', (chunk) => scan(chunk.toString('utf8')));
        child.once('exit', (code) => {
            clearTimeout(timer);
            reject(new Error(`browser exited with code ${code}:\n${output}`));
        });
    });

    const socketUrl = await banner;
    const port = Number.parseInt(new URL(socketUrl).port, 10);
    return {
        process: child,
        port,
        stderr: () => output,
        /** Kill the browser without waiting (node:test teardown). */
        release: () => {
            try {
                child.kill('SIGKILL');
            } catch { /* already gone */ }
        },
        close: async () => {
            if (child.exitCode !== null || child.signalCode !== null) return;
            const exited = new Promise((resolve) => child.once('exit', resolve));
            child.kill('SIGTERM');
            const timer = setTimeout(() => child.kill('SIGKILL'), 3000);
            timer.unref?.();
            await exited;
            clearTimeout(timer);
        },
    };
}

/** Poll the DevTools HTTP endpoint until a page target exists. */
async function pageTarget(port, timeoutMs = 10000) {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
        try {
            const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
            const page = list.find((target) => target.type === 'page');
            if (page?.webSocketDebuggerUrl) return page;
        } catch {
            // The HTTP endpoint comes up with the banner; a single miss is not
            // an error.
        }
        if (Date.now() > deadline) throw new Error('no CDP page target appeared');
        await new Promise((resolve) => setTimeout(resolve, 50));
    }
}

/**
 * Connect to the browser's first page target.
 *
 * @returns {Promise<{send: Function, evaluate: Function, on: Function,
 *                    exceptions: string[], close: Function}>}
 */
export async function connectPage(port) {
    if (typeof WebSocket === 'undefined') {
        throw new Error('this Node has no global WebSocket; CDP needs Node 22 or newer');
    }
    const target = await pageTarget(port);
    const socket = new WebSocket(target.webSocketDebuggerUrl);
    const pending = new Map();
    const listeners = new Map();
    const exceptions = [];
    let nextId = 0;

    socket.addEventListener('message', (event) => {
        const message = JSON.parse(event.data);
        if (message.id && pending.has(message.id)) {
            const { resolve, reject } = pending.get(message.id);
            pending.delete(message.id);
            if (message.error) reject(new Error(`${message.error.message} (${message.method})`));
            else resolve(message.result);
            return;
        }
        if (message.method === 'Runtime.exceptionThrown') {
            const details = message.params.exceptionDetails;
            exceptions.push(details.exception?.description ?? details.text);
        }
        for (const handler of listeners.get(message.method) ?? []) handler(message.params);
    });
    await new Promise((resolve, reject) => {
        socket.addEventListener('open', resolve, { once: true });
        socket.addEventListener('error', () => reject(new Error('CDP socket failed')), { once: true });
    });

    const send = (method, params = {}) => new Promise((resolve, reject) => {
        const id = (nextId += 1);
        pending.set(id, { resolve, reject });
        socket.send(JSON.stringify({ id, method, params }));
    });

    return {
        exceptions,
        send,
        on: (method, handler) => {
            if (!listeners.has(method)) listeners.set(method, []);
            listeners.get(method).push(handler);
        },
        /** Evaluate an expression in the page and return its value. */
        evaluate: async (expression) => {
            const result = await send('Runtime.evaluate', {
                expression,
                returnByValue: true,
                awaitPromise: true,
            });
            if (result.exceptionDetails) {
                throw new Error(`page evaluation failed: ${result.exceptionDetails.text}`
                    + ` ${result.exceptionDetails.exception?.description ?? ''}`);
            }
            return result.result?.value;
        },
        close: () => socket.close(),
    };
}
