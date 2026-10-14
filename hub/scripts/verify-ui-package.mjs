/** Bootstrap checks even on publish/recovery runners with production-only dependencies. */
import { execFileSync } from 'node:child_process';
import { copyFileSync, cpSync, mkdirSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

export function verifyUiPackage(archive, installed) {
    const root = mkdtempSync(join(tmpdir(), 'simplex-ui-package-'));
    const source = fileURLToPath(new URL('../', import.meta.url));
    try {
        const toolchain = join(root, 'toolchain');
        mkdirSync(join(toolchain, 'scripts'), { recursive: true });
        for (const name of ['package.json', 'package-lock.json']) copyFileSync(join(source, name), join(toolchain, name));
        // Explicit --include=dev also handles NODE_ENV=production. These tools
        // stay outside both consumer fixtures and the published package. The
        // repository lockfile pins their complete transitive dependency graph.
        execFileSync('npm', ['ci', '--include=dev', '--ignore-scripts', '--prefer-offline', '--no-audit', '--no-fund'], {
            cwd: toolchain, stdio: 'inherit', timeout: 180_000,
        });
        for (const name of ['check-ui-package.mjs', 'check-ui-boundary.mjs']) {
            copyFileSync(join(source, 'scripts', name), join(toolchain, 'scripts', name));
        }
        cpSync(join(source, 'test/fixtures/ui-sdk'), join(toolchain, 'fixtures'), { recursive: true });
        execFileSync(process.execPath, [join(toolchain, 'scripts/check-ui-package.mjs'), archive, installed, join(root, 'consumers')], {
            cwd: root, stdio: 'inherit', timeout: 120_000,
        });
    } finally {
        rmSync(root, { recursive: true, force: true });
    }
}
