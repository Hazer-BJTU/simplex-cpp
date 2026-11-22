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
    // Repository indexes, internal references, and historical notes stay in Git
    // without becoming public pages or local-search entries.
    srcExclude: [
        '**/README.md',
        'architecture/sqlite-schema-v1.md',
        'deployment/remote-worker.md',
        'hub/npm-release.md',
        'abi-context.md',
        'plugins/development.md',
        'plugins/tools.md',
        'plugins/loop-hooks.md',
        'plugins/model-providers.md',
        'maintaining/documentation.md',
        'hub/panel-redesign-plan.md',
        'node_modules/**',
    ],
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
                ['Persistence backend', '/architecture/persistence-backends'],
                ['Security', '/architecture/security'],
            ]),
            group('Build from source', [
                ['Local environment', '/building/local'],
                ['Build images', '/building/docker'],
            ]),
            group('Deployment', [
                ['Deploy the Hub and a worker', '/deployment/hub'],
            ]),
            group('Protocol', [
                ['Simplex Loop Worker Protocol', '/core/worker-protocol'],
                ['Hub UI SDK', '/hub/ui-sdk'],
            ]),
            group('Plugins', [
                ['Plugin roles', '/plugins/overview'],
                ['Configuration', '/plugins/configuration'],
                ['Programmatic tool calling', '/plugins/ptc'],
            ]),
            group('Providers', [
                ['DeepSeek', '/providers/deepseek'],
                ['Qwen', '/providers/qwen'],
                ['GLM (Zhipu AI)', '/providers/glm'],
                ['Custom Chat Completions', '/providers/chat-completions'],
            ]),
            {
                text: 'Reference',
                collapsed: false,
                items: [
                    { text: 'Command line', link: '/reference/cli' },
                    {
                        ...group('Configuration library', [
                            ['Hub settings', '/reference/hub-settings'],
                            ['Launch profiles', '/reference/launch-profiles'],
                            ['Worker settings', '/reference/worker-settings'],
                        ]),
                        link: '/hub/configurations',
                    },
                ],
            },
        ],
        search: { provider: 'local' },
        outline: [2, 3],
        editLink: { pattern: 'https://github.com/Hazer-BJTU/simplex-cpp/edit/main/docs/:path' },
        socialLinks: [{ icon: 'github', link: 'https://github.com/Hazer-BJTU/simplex-cpp' }],
        footer: { message: `Documentation tracks main (repository version ${version}); unreleased changes may differ from published packages.` },
    },
});
