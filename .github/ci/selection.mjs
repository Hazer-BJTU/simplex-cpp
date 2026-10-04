/** Pure path rules and job decisions for ordinary PR/main CI. */
import { isProjectVersion } from './version-only.mjs';

const nativeTrees = new Set([
    'core', 'dataclass', 'endpoint', 'extensions', 'intercom', 'io', 'llm',
    'load', 'loop', 'process', 'textedit', 'tools', 'utils', 'versioning',
    'third_party', 'cmake',
]);

const harmlessRootFiles = new Set([
    'README.md', 'CONTRIBUTING.md', 'SECURITY.md', 'LICENSE', '.gitignore',
]);
const outOfScopeWorkflows = new Set([
    '.github/workflows/docs.yml', '.github/workflows/release-worker.yml',
]);

/** Unknown paths deliberately select native validation. No extension exclusions. */
export function categoryForPath(path) {
    if (typeof path !== 'string' || !path || path.startsWith('/')
        || path.split('/').some((part) => !part || part === '.' || part === '..')) {
        throw new Error('invalid repository-relative changed path');
    }
    const root = path.split('/')[0];
    if (nativeTrees.has(root) || root === 'docker'
        || path === 'CMakeLists.txt' || path === 'VERSION'
        || path.startsWith('.github/ci/') || path === '.github/workflows/ci.yml') {
        return 'native';
    }
    if (path.startsWith('hub/web/') || path.startsWith('hub/test/browser/')) {
        return 'panel';
    }
    // Backend, shared contracts, fixtures, dependencies and configuration all
    // conservatively require a freshly built worker for integration testing.
    if (path.startsWith('hub/')) {
        return 'integration';
    }
    if (path.startsWith('docs/') || path.startsWith('assets/')
        || path.startsWith('.github/ISSUE_TEMPLATE/')
        || path === '.github/PULL_REQUEST_TEMPLATE.md'
        || harmlessRootFiles.has(path) || outOfScopeWorkflows.has(path)) {
        return 'documentation';
    }
    return 'native';
}

/** Compute one category first, then derive all producer/consumer decisions. */
export function classifyPaths(paths) {
    if (!Array.isArray(paths)) {
        throw new Error('changed paths must be an array');
    }
    const categories = paths.map(categoryForPath);
    const category = categories.includes('native') ? 'native'
        : categories.includes('integration') ? 'integration' : 'independent';
    return decisionsForCategory(category);
}

export function decisionsForCategory(category, transition) {
    if (!['native', 'integration', 'independent', 'version-only'].includes(category)) {
        throw new Error(`unknown CI category: ${category}`);
    }
    const usesWorkerBuild = category === 'native' || category === 'integration';
    const decisions = {
        category,
        cpp_validation: category === 'native',
        portable_build: usesWorkerBuild,
        staged_validation: category === 'native',
        worker_integration: usesWorkerBuild,
    };
    if (category === 'version-only') {
        if (!isProjectVersion(transition?.from) || !isProjectVersion(transition?.to)
            || transition.from === transition.to) {
            throw new Error('version-only selection requires a valid version transition');
        }
        decisions.version_from = transition.from;
        decisions.version_to = transition.to;
    }
    return decisions;
}

/** Validate output before writing it or trusting it at the final gate. */
export function validateDecisions(decisions) {
    const expected = decisionsForCategory(decisions.category, {
        from: decisions.version_from, to: decisions.version_to,
    });
    for (const [key, value] of Object.entries(expected)) {
        if (decisions[key] !== value) {
            throw new Error(`inconsistent CI selection: ${key}`);
        }
    }
    if (decisions.category !== 'version-only'
        && (decisions.version_from || decisions.version_to)) {
        throw new Error('unexpected version transition for ordinary path selection');
    }
    return decisions;
}
