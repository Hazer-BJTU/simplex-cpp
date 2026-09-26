/**
 * @file test helper: start a hub on an ephemeral port with a scratch data dir.
 */
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { hubRoot, loadConfig } from '../../src/config.ts';
import { createHub } from '../../src/hub.js';
import { createLogger } from '../../src/log.ts';

/** Logger level for tests: silent unless HUB_TEST_LOG names one. */
export function testLogLevel() {
    return process.env.HUB_TEST_LOG ?? 'silent';
}

/** Merge test defaults into hub configuration overrides. */
export function testConfig(overrides = {}) {
    const { config } = loadConfig({
        overrides: {
            listen: { host: '127.0.0.1', port: 0 },
            dataDir: mkdtempSync(join(tmpdir(), 'simplex-hub-test-')),
            ...overrides,
        },
    });
    return config;
}

/**
 * Start a hub for one test file.
 *
 * @param {object} [overrides] hub configuration overrides.
 * @param {object} [hooks] hub observer hooks (onEvent, onPrompt, ...).
 * @returns {Promise<{hub: object, base: string, wsBase: string, port: number}>}
 */
export async function startTestHub(overrides = {}, hooks = {}) {
    const config = testConfig(overrides);
    const hub = createHub({
        config,
        log: createLogger({ level: testLogLevel() }),
        hubRoot,
        version: 'test',
        hooks,
    });
    const address = await hub.start();
    return {
        hub,
        config,
        port: address.port,
        base: `http://127.0.0.1:${address.port}`,
        wsBase: `ws://127.0.0.1:${address.port}`,
    };
}
