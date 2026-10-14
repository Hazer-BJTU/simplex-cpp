/** Structural contract boundary, including declarations not reachable from the barrel. */
import { parse } from '@babel/parser';
import { readFileSync, readdirSync, realpathSync } from 'node:fs';
import { dirname, resolve, relative, isAbsolute } from 'node:path';
import { fileURLToPath } from 'node:url';

export function checkUiBoundary(directory) {
    const root = realpathSync(directory);
    const visited = new Set();
    function inspect(file) {
        file = realpathSync(file);
        const rel = relative(root, file);
        if (rel.startsWith('..') || isAbsolute(rel)) throw new Error(`SDK dependency escapes ui/: ${file}`);
        if (visited.has(file)) return;
        visited.add(file);
        const source = parse(readFileSync(file, 'utf8'), { sourceType: 'module', plugins: ['typescript'] });
        const constants = new Set();
        for (const node of source.program.body) {
            const declaration = node.declaration ?? node;
            if (declaration.type === 'VariableDeclaration') {
                for (const item of declaration.declarations) if (item.id.type === 'Identifier') constants.add(item.id.name);
            }
            if (node.type === 'ImportDeclaration' && node.importKind !== 'type') {
                for (const item of node.specifiers) if (item.importKind !== 'type') constants.add(item.local.name);
            }
        }
        const fail = (node, message) => { throw new Error(`${rel}:${node.loc?.start.line}: ${message}`); };
        if (source.comments.some(comment => /<reference\s/.test(comment.value))) fail(source, 'Ambient reference directive');
        function dependency(node, value) {
            if (!value.startsWith('./') && !value.startsWith('../')) fail(node, 'External SDK dependency');
            inspect(resolve(dirname(file), value));
        }
        // A positive expression list excludes calls, accessors, spreads,
        // functions, computed keys, mutation and executable initialization.
        function constant(node) {
            if (['StringLiteral', 'NumericLiteral', 'BooleanLiteral', 'NullLiteral'].includes(node.type)) return;
            if (node.type === 'Identifier' && constants.has(node.name)) return;
            if (node.type === 'MemberExpression' && !node.computed && node.property.type === 'Identifier') return constant(node.object);
            if (['TSAsExpression', 'TSSatisfiesExpression', 'TSNonNullExpression'].includes(node.type)) return constant(node.expression);
            if (node.type === 'ArrayExpression') { node.elements.forEach(constant); return; }
            if (node.type === 'ObjectExpression') {
                for (const property of node.properties) {
                    if (property.type !== 'ObjectProperty' || property.computed) fail(property, 'Non-static constant property');
                    constant(property.value);
                }
                return;
            }
            fail(node, 'Executable SDK initializer');
        }
        function types(node) {
            if (!node || typeof node !== 'object') return;
            if (node.type === 'TSImportType') dependency(node, node.argument.value);
            for (const [key, value] of Object.entries(node)) {
                if (['loc', 'comments', 'tokens'].includes(key)) continue;
                if (Array.isArray(value)) value.forEach(types);
                else if (value && typeof value === 'object') types(value);
            }
        }
        function statement(node) {
            switch (node.type) {
                case 'ImportDeclaration':
                    if (!node.specifiers.length) fail(node, 'Side-effect import');
                    dependency(node, node.source.value);
                    return;
                case 'ExportNamedDeclaration':
                    if (node.source) dependency(node, node.source.value);
                    if (node.declaration) statement(node.declaration);
                    return;
                case 'TSInterfaceDeclaration':
                case 'TSTypeAliasDeclaration':
                    return;
                case 'VariableDeclaration':
                    if (node.kind !== 'const' || node.declare) fail(node, 'Only initialized static constants are allowed');
                    for (const declaration of node.declarations) {
                        if (declaration.id.type !== 'Identifier' || !declaration.init) fail(declaration, 'Invalid SDK constant');
                        constant(declaration.init);
                    }
                    return;
                default:
                    fail(node, 'Only types, explicit exports and static constants belong in the SDK');
            }
        }
        for (const node of source.program.body) { types(node); statement(node); }
    }
    function walk(directory) {
        for (const entry of readdirSync(directory, { withFileTypes: true })) {
            const path = resolve(directory, entry.name);
            if (entry.isDirectory()) walk(path);
            else if (entry.name.endsWith('.ts')) inspect(path);
        }
    }
    walk(root);
    return visited;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    const files = checkUiBoundary(fileURLToPath(new URL('../ui', import.meta.url)));
    console.log(`UI contract boundary checked: ${files.size} modules`);
}
