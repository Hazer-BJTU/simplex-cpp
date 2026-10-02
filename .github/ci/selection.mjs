/** Pure path rules and job decisions for ordinary PR/main CI. */
const nativeTrees = new Set([
    'core', 'dataclass', 'endpoint', 'extensions', 'intercom', 'io', 'llm',
    'load', 'loop', 'process', 'textedit', 'tools', 'utils', 'versioning',
    'third_party', 'cmake',
]);

const harmlessRootFiles = new Set(['README.md', 'LICENSE', '.gitignore']);
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

export function decisionsForCategory(category) {
    if (!['native', 'integration', 'independent'].includes(category)) {
        throw new Error(`unknown CI category: ${category}`);
    }
    return {
        category,
        cpp_validation: category === 'native',
        portable_build: category !== 'independent',
        staged_validation: category === 'native',
        worker_integration: category !== 'independent',
    };
}

/** Validate output before writing it or trusting it at the final gate. */
export function validateDecisions(decisions) {
    const expected = decisionsForCategory(decisions.category);
    for (const [key, value] of Object.entries(expected)) {
        if (decisions[key] !== value) {
            throw new Error(`inconsistent CI selection: ${key}`);
        }
    }
    return decisions;
}
