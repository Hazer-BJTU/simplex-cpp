import { defineConfig } from 'vitepress';
import { readFileSync } from 'node:fs';

const version = readFileSync(new URL('../../VERSION', import.meta.url), 'utf8').trim();
const group = (text: string, entries: [string, string][]) => ({
    text,
    collapsed: false,
    items: entries.map(([text, link]) => ({ text, link })),
});

export default defineConfig({
    lang: 'en-US',
    title: 'Simplex',
    description: 'Understand every detail of an agent harness. Native C++ workers, extensible tools, and a WebSocket Hub.',
    base: '/simplex-cpp/',
    // Historical notes remain in Git; they are not current English user guides.
    srcExclude: ['**/README.md', 'hub/panel-redesign-plan.md', 'node_modules/**'],
    lastUpdated: true,
    themeConfig: {
        nav: [
            { text: 'Get started', link: '/getting-started/installation' },
            { text: 'Protocol', link: '/core/worker-protocol' },
            { text: `main · ${version}`, link: 'https://github.com/Hazer-BJTU/simplex-cpp' },
        ],
        sidebar: [
            group('Getting started', [
                ['Installation', '/getting-started/installation'],
                ['Configuration', '/getting-started/configuration'],
            ]),
            group('Architecture', [
                ['Overview', '/architecture/overview'],
                ['Agent loop', '/architecture/agent-loop'],
                ['State and persistence', '/architecture/state-and-persistence'],
                ['Security', '/architecture/security'],
            ]),
            group('Build from source', [
                ['Local environment', '/building/local'],
                ['Build images', '/building/docker'],
            ]),
            group('Deployment', [
                ['Hub and worker lifecycle', '/deployment/hub'],
                ['Docker worker', '/deployment/docker-worker'],
                ['Remote worker', '/deployment/remote-worker'],
            ]),
            group('Protocol', [
                ['Simplex Loop Worker Protocol', '/core/worker-protocol'],
                ['Hub panel protocol', '/hub/hub-protocol'],
                ['Headless subagents', '/hub/subagents'],
            ]),
            group('Plugins', [
                ['Principles', '/plugins/overview'],
                ['Configuration', '/plugins/configuration'],
                ['Development workflow', '/plugins/development'],
                ['Tool plugins', '/plugins/tools'],
                ['Loop hooks', '/plugins/loop-hooks'],
                ['Model providers', '/plugins/model-providers'],
            ]),
            group('Providers', [
                ['Support policy', '/providers/index'],
                ['DeepSeek', '/providers/deepseek'],
                ['Qwen', '/providers/qwen'],
            ]),
            group('Reference', [
                ['Command line', '/reference/cli'],
                ['Configuration library', '/hub/configurations'],
                ['npm releases', '/hub/npm-release'],
                ['Maintaining the docs', '/maintaining/documentation'],
            ]),
        ],
        search: { provider: 'local' },
        outline: [2, 3],
        editLink: { pattern: 'https://github.com/Hazer-BJTU/simplex-cpp/edit/main/docs/:path' },
        socialLinks: [{ icon: 'github', link: 'https://github.com/Hazer-BJTU/simplex-cpp' }],
        footer: { message: `Documentation tracks main (repository version ${version}); unreleased changes may differ from published packages.` },
    },
});
