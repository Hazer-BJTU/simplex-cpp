/**
 * @file panel protocol drift: the hub's vocabulary against its own document.
 *
 * `docs/hub-protocol.md` is the prose contract for the browser protocol, and
 * `shared/protocol.ts` is the machine-readable copy the hub and the panel both
 * import. The worker side has had a drift check against
 * `core/docs/worker-protocol.md` for a while; the panel side had none, so the
 * document and the code could disagree with nothing to notice.
 *
 * The failure this prevents is quiet: a message type the document promises and
 * the hub never accepts, an error code a client switches on that no longer
 * exists, or a type the hub emits that nobody wrote down. All of those keep the
 * tests green and only show up as a panel feature that never works.
 *
 * When the document is reformatted beyond what this parser understands, the
 * test fails too — with a message that says which of the two to fix.
 */
import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { describe, it } from 'node:test';
import { hubRoot } from '../src/config.ts';
import {
    CAPABILITIES,
    ERROR_CODES,
    HUB_MESSAGE_TYPES,
    PANEL_MESSAGE_TYPES,
} from '../shared/protocol.ts';

const DOC_PATH = join(hubRoot, 'docs', 'hub-protocol.md');
const available = existsSync(DOC_PATH);
const skip = available ? false : `hub documentation not present at ${DOC_PATH}`;

/** Hint appended to every parse failure. */
const PARSER_HINT = 'The document was reformatted or the vocabulary moved. Update this '
    + "test's parser, or shared/protocol.ts, to match — do not delete the check.";

/** Read the authoritative document. */
function readDocument() {
    return readFileSync(DOC_PATH, 'utf8');
}

/**
 * Slice out one section: from its heading to the next heading of the same or a
 * higher level.
 *
 * @param {string} markdown
 * @param {string} heading the heading line, including its `#` markers.
 */
function section(markdown, heading) {
    const start = markdown.indexOf(heading);
    assert.notEqual(start, -1, `hub-protocol.md has no "${heading}" section. ${PARSER_HINT}`);
    const body = markdown.slice(start + heading.length);
    const next = body.search(/\n#{2,3} /);
    return next === -1 ? body : body.slice(0, next);
}

/**
 * First column of every table row in a section, in order.
 *
 * Backticks are stripped, so a cell written as `` `subscribe` `` and one written
 * as `subscribe` read the same. The `---` separator and the header row are
 * dropped: the header names are listed rather than guessed, because "skip the
 * first row" breaks the moment a section holds two tables.
 */
const TABLE_HEADERS = new Set(['Message', 'Code', 'Capability', 'Surface', 'State', 'Method and path']);

function firstColumn(markdown, heading) {
    const names = [];
    for (const line of section(markdown, heading).split('\n')) {
        if (!line.startsWith('|')) continue;
        const cells = line.split('|').slice(1, -1).map((cell) => cell.trim());
        const first = cells[0];
        if (first === undefined || first.length === 0) continue;
        if (/^-+$/.test(first)) continue;
        if (TABLE_HEADERS.has(first)) continue;
        names.push(first.replaceAll('`', ''));
    }
    assert.ok(names.length > 0, `no table rows found under "${heading}". ${PARSER_HINT}`);
    return names;
}

/** Compare a documented list against the code's, ignoring order. */
function assertSameVocabulary(documented, declared, label) {
    const sorted = (list) => [...list].sort();
    assert.deepEqual(sorted(documented), sorted(declared),
        `${label} in hub-protocol.md and shared/protocol.ts disagree`);
}

describe('panel protocol drift', { skip }, () => {
    it('documents exactly the message types the hub accepts', () => {
        assertSameVocabulary(
            firstColumn(readDocument(), '### Client to hub'),
            PANEL_MESSAGE_TYPES,
            'the client-to-hub message types');
    });

    it('documents exactly the message types the hub emits', () => {
        assertSameVocabulary(
            firstColumn(readDocument(), '### Hub to client'),
            HUB_MESSAGE_TYPES,
            'the hub-to-client message types');
    });

    it('documents exactly the error codes the hub sends', () => {
        assertSameVocabulary(
            firstColumn(readDocument(), '### Error codes'),
            ERROR_CODES,
            'the error codes');
    });

    it('documents exactly the capabilities the hub advertises', () => {
        assertSameVocabulary(
            firstColumn(readDocument(), '### Capabilities'),
            CAPABILITIES,
            'the capabilities');
    });

    it('documents the transcript epoch the code now reports', () => {
        // The field is what turns a stale replay cursor into a signal instead of
        // an empty transcript, so a client has to be able to read about it.
        assert.match(readDocument(), /transcript_epoch/,
            'hub-protocol.md does not mention transcript_epoch');
    });

    it('documents that confirmations reach every client', () => {
        assert.match(readDocument(), /every (connected )?(panel )?client|not only .*subscrib/i,
            'hub-protocol.md does not state who receives a confirmation');
    });
});
