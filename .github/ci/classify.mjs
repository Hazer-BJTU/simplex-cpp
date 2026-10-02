/** GitHub Actions entry point; uncertain ranges select the complete native chain. */
import { appendFileSync, readFileSync } from 'node:fs';
import { resolveRange, collectChangedPaths } from './changes.mjs';
import { classifyPaths, decisionsForCategory, validateDecisions } from './selection.mjs';

let selection;
let explanation;
try {
    const event = JSON.parse(readFileSync(process.env.GITHUB_EVENT_PATH, 'utf8'));
    const range = resolveRange(process.cwd(), process.env.GITHUB_EVENT_NAME,
        event, process.env.GITHUB_SHA);
    const paths = collectChangedPaths(process.cwd(), range);
    selection = classifyPaths(paths);
    explanation = `${paths.length} changed path(s), ${range.base} → ${range.head}`;
    for (const path of paths) {
        console.log(JSON.stringify(path));
    }
} catch (error) {
    selection = decisionsForCategory('native');
    // JSON encoding prevents unusual filenames/error text from becoming Actions
    // workflow commands or multiline summary markup.
    explanation = `Full-validation fallback: ${JSON.stringify(error.message)}`;
}
// Rule/output inconsistencies are programmer errors: fail rather than silently
// converting an invalid producer/consumer combination to a different selection.
validateDecisions(selection);
console.log(explanation);
console.log(JSON.stringify(selection));
if (!process.env.GITHUB_OUTPUT || !process.env.GITHUB_STEP_SUMMARY) {
    throw new Error('GitHub Actions output/summary paths are required');
}
appendFileSync(process.env.GITHUB_OUTPUT,
    Object.entries(selection).map(([key, value]) => `${key}=${value}\n`).join(''));
appendFileSync(process.env.GITHUB_STEP_SUMMARY,
    `## CI selection\n\n${explanation.replaceAll('<', '&lt;').replaceAll('>', '&gt;')}\n\n`
    + `Category: **${selection.category}**\n\n`
    + '| Decision | Selected |\n| --- | --- |\n'
    + Object.entries(selection).filter(([key]) => key !== 'category')
        .map(([key, value]) => `| ${key} | ${value} |\n`).join(''));
