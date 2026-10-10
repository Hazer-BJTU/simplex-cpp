/**
 * @file protocol drift: the hub's vocabulary against the core document.
 *
 * `docs/core/worker-protocol.md` is the authoritative contract, and the hub
 * keeps its own copies of that vocabulary (the event table, the signal
 * operations, the accepted content types and option categories). A copy can
 * drift silently: the hub would keep working, its tests would keep passing, and
 * the only symptom would be a feature that never lights up — a new event
 * rendered as "unknown", a new signal the panel cannot send.
 *
 * So the document itself is parsed here. When core adds an event or a signal,
 * this test fails until the hub is taught about it. When the document is
 * reformatted beyond what this parser understands, it fails too — with a
 * message that says which of the two to fix.
 *
 * It reads the repository's documentation and, for Worker-owned fields, the
 * C++ emission/serialization sites. A standalone copy of `hub/` without those
 * files skips the corresponding checks instead of failing.
 */
import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { describe, it } from 'node:test';
import { hubRoot } from '../src/config.ts';
import { EVENT_TABLE, KNOWN_EVENTS } from '../src/protocol/events.ts';
import {
    CONFIRMATION_MODES,
    CONTENT_MODALITIES,
    CONTENT_TYPES,
    INPUT_OPERATIONS,
    SIGNAL_OPERATIONS,
    buildConfirmationResponse,
    buildPayload,
    buildSignal,
    normalizeOptions,
} from '../src/protocol/messages.ts';

const DOC_PATH = join(hubRoot, '..', 'docs', 'core', 'worker-protocol.md');
const available = existsSync(DOC_PATH);
const skip = available ? false : `core documentation not present at ${DOC_PATH}`;
const WORKER_PATH = join(hubRoot, '..', 'core', 'src', 'application.cpp');
const MODEL_IO_PATH = join(hubRoot, '..', 'dataclass', 'include', 'dataclass', 'model_io.hpp');
const OUTBOX_PATH = join(hubRoot, '..', 'core', 'include', 'core', 'event_outbox.hpp');
const workerSkip = [WORKER_PATH, MODEL_IO_PATH, OUTBOX_PATH].every(existsSync)
    ? false : 'Worker C++ sources are not present in this standalone Hub';

/** Hint appended to every parse failure. */
const PARSER_HINT = 'The document was reformatted or the vocabulary moved. Update this '
    + "test's parser, or the hub, to match — do not delete the check.";

/** Read the authoritative document. */
function readDocument() {
    return readFileSync(DOC_PATH, 'utf8');
}

/**
 * Slice out one section: from the heading line to the next heading of the same
 * or a higher level.
 */
function section(markdown, heading) {
    const lines = markdown.split('\n');
    const level = heading.match(/^#+/)[0].length;
    const start = lines.findIndex((line) => line.trim() === heading);
    assert.notEqual(start, -1, `section "${heading}" not found in worker-protocol.md. ${PARSER_HINT}`);
    let end = lines.length;
    for (let index = start + 1; index < lines.length; index += 1) {
        const match = /^(#+)\s/.exec(lines[index]);
        if (match && match[1].length <= level) {
            end = index;
            break;
        }
    }
    return lines.slice(start + 1, end).join('\n');
}

/** Parse every markdown table in a slice into rows of trimmed cells. */
function tables(text) {
    const found = [];
    let current = null;
    for (const line of text.split('\n')) {
        const trimmed = line.trim();
        if (!trimmed.startsWith('|')) {
            current = null;
            continue;
        }
        const cells = trimmed.replace(/^\|/, '').replace(/\|$/, '').split('|').map((c) => c.trim());
        if (cells.every((cell) => /^:?-{2,}:?$/.test(cell))) continue;
        if (!current) {
            current = [];
            found.push(current);
        }
        current.push(cells);
    }
    return found;
}

/** The first table of a section, header row included. */
function firstTable(text, label) {
    const [table] = tables(text);
    assert.ok(table && table.length > 1, `no table found for ${label}. ${PARSER_HINT}`);
    return table.slice(1);
}

/** Identifiers inside backticks in one cell. */
function backticks(cell) {
    return [...cell.matchAll(/`([^`]+)`/g)].map((match) => match[1]);
}

/** First backticked identifier of a cell, without any type suffix. */
function identifier(cell) {
    const [first] = backticks(cell);
    return first?.replace(/:.*$/, '') ?? cell.replace(/`/g, '');
}

/** Extract a known source fragment, failing explicitly when its layout changes. */
function sourceFragment(text, pattern, label) {
    const match = pattern.exec(text);
    assert.ok(match, `cannot locate ${label} in the C++ source. ${PARSER_HINT}`);
    return match[1];
}

describe('worker protocol drift', { skip }, () => {
    it('keeps status capability examples and glossary aligned with the Worker', { skip: workerSkip }, () => {
        const worker = readFileSync(WORKER_PATH, 'utf8');
        const capabilities = sourceFragment(worker,
            /\{"capabilities", Json::array\(\{([^}]+)\}\)\}/, 'status capabilities');
        const supported = [...capabilities.matchAll(/"([a-z-]+)"/g)].map(match => match[1]).sort();
        assert.ok(supported.length > 0, PARSER_HINT);
        const status = section(readDocument(), '### Status object');
        const example = JSON.parse(sourceFragment(status, /```json\s*([\s\S]*?)```/, 'status JSON example'));
        assert.deepEqual(example.capabilities.sort(), supported);
        const rows = firstTable(status, 'status fields');
        const row = rows.find(entry => identifier(entry[0]) === 'capabilities');
        assert.ok(row, PARSER_HINT);
        const described = backticks(row[2]).filter(value => supported.includes(value)).sort();
        assert.deepEqual(described, supported);
        assert.match(row[2], /not necessarily enabled/);
        assert.match(row[2], /auto_compact\.threshold/);
        assert.equal(example.auto_compact.threshold, 0);
        assert.ok(rows.some(entry => identifier(entry[0]) === 'auto_compact'));
    });

    it('lists every persisted loop status serialized by model_io', { skip: workerSkip }, () => {
        const modelIO = readFileSync(MODEL_IO_PATH, 'utf8');
        const serialization = sourceFragment(modelIO,
            /inline void to_json\(nlohmann::json& j, LoopStatus value\) \{([\s\S]*?)\n\}/, 'LoopStatus serialization');
        const serialized = [...serialization.matchAll(/j = "([a-z_]+)";/g)].map(match => match[1]).sort();
        assert.ok(serialized.length > 0, PARSER_HINT);
        const rows = tables(section(readDocument(), '### Status object')).flat();
        const row = rows.find(entry => identifier(entry[0]) === 'status');
        assert.ok(row, PARSER_HINT);
        assert.deepEqual(backticks(row[1]).sort(), serialized);
        assert.match(row[2], /not a queued action/);
    });

    it('documents all run_finished emission fields and optional outbox counters', { skip: workerSkip }, () => {
        const worker = readFileSync(WORKER_PATH, 'utf8');
        const finished = sourceFragment(worker, /Json finished = \{([\s\S]*?)\};/, 'run_finished data');
        const required = [...finished.matchAll(/\{"([a-z_]+)",/g)].map(match => match[1]);
        assert.ok(required.length > 0, PARSER_HINT);
        const conditional = [...worker.matchAll(/finished\["([a-z_]+)"\]/g)].map(match => match[1]);
        const outbox = readFileSync(OUTBOX_PATH, 'utf8');
        const counters = [...outbox.matchAll(/message\["data"\]\["([a-z_]+)"\]/g)].map(match => match[1]);
        assert.ok(conditional.length > 0 && counters.length > 0, PARSER_HINT);
        const optional = [...new Set([...conditional, ...counters])];
        const rows = firstTable(section(readDocument(), '### Run outcome and persistence events'), 'run outcome fields');
        const documented = rows.map(row => identifier(row[0]));
        assert.deepEqual(documented.sort(), [...required, ...optional].sort());
        for (const row of rows) {
            assert.equal(row[1].startsWith('Optional'), optional.includes(identifier(row[0])),
                `incorrect optionality for ${row[0]}`);
        }
    });

    it('documents hub events plus explicit worker-only additions', () => {
        const rows = firstTable(section(readDocument(), '## Worker events'), 'the worker events table');
        const documented = rows.map((row) => identifier(row[0])).sort();
        assert.deepEqual([...KNOWN_EVENTS].sort(), documented,
            'the hub\'s event vocabulary and docs/core/worker-protocol.md disagree');
        // Every hub-supported event needs a rendering hint. Worker-only events
        // use the existing unknown-event display until the follow-up rollout.
        for (const name of documented) {
            assert.ok(Object.hasOwn(EVENT_TABLE, name), `no rendering hint for ${name}`);
        }
    });

    it('documents exactly the signals the hub can send', () => {
        const rows = firstTable(section(readDocument(), '## Control signals'), 'the control signals table');
        const documented = rows.map((row) => identifier(row[0])).sort();
        assert.deepEqual([...SIGNAL_OPERATIONS].sort(), documented);
        for (const operation of documented) {
            assert.doesNotThrow(() => buildSignal({ operation, runId: 'run-1' }),
                `the hub cannot build the documented signal "${operation}"`);
        }
    });

    it('documents the input operations accepted by the hub', () => {
        const requests = section(readDocument(), '## User requests');
        const documented = [...requests.matchAll(/"operation"\s*:\s*"([a-z_]+)"/g)]
            .map((match) => match[1]);
        assert.ok(documented.length > 0, `no input operations found. ${PARSER_HINT}`);
        assert.deepEqual([...INPUT_OPERATIONS].sort(), [...new Set(documented)].sort());
        assert.doesNotThrow(() => buildPayload({
            requestId: 'r', content: [{ type: 'text', raw: 'x', modality: 'text' }] }));
        assert.doesNotThrow(() => buildPayload({ operation: 'continue', requestId: 'r' }));
    });

    it('documents exactly the content encodings the hub accepts', () => {
        const rows = firstTable(section(readDocument(), '### Content'), 'the Content table');
        const typeRow = rows.find((row) => identifier(row[0]) === 'type');
        assert.ok(typeRow, `no "type" row in the Content table. ${PARSER_HINT}`);
        const documented = backticks(typeRow[1]).sort();
        assert.deepEqual([...CONTENT_TYPES].sort(), documented);
    });

    it('documents exactly the modalities the hub accepts', () => {
        const rows = firstTable(section(readDocument(), '### Content'), 'the Content table');
        const modalityRow = rows.find((row) => identifier(row[0]) === 'modality');
        assert.ok(modalityRow, `no "modality" row in the Content table. ${PARSER_HINT}`);
        const documented = backticks(modalityRow[1]).sort();
        assert.deepEqual([...CONTENT_MODALITIES].sort(), documented);
        // A part without the label is refused rather than assumed to be text.
        assert.throws(
            () => buildPayload({ requestId: 'r', content: [{ type: 'text', raw: 'x' }] }),
            /modality/);
    });

    it('documents exactly the option categories the hub accepts', () => {
        const rows = firstTable(
            section(readDocument(), '### Apply options at the next run boundary'),
            'the payload options table');
        const documented = rows.map((row) => identifier(row[0]));
        assert.deepEqual(documented.sort(), ['confirmation', 'model', 'tools']);
        for (const category of documented) {
            const value = category === 'confirmation' ? {} : category === 'tools' ? {} : {};
            assert.doesNotThrow(() => normalizeOptions({ [category]: value }),
                `the hub refuses the documented option category "${category}"`);
        }
        assert.throws(() => normalizeOptions({ not_documented: {} }), /unknown options category/);
    });

    it('documents exactly the confirmation modes the hub can select', () => {
        const rows = firstTable(
            section(readDocument(), '### Apply options at the next run boundary'),
            'the payload options table');
        const row = rows.find((entry) => identifier(entry[0]) === 'confirmation');
        assert.ok(row, `no confirmation row in the options table. ${PARSER_HINT}`);
        // "Object with optional `mode`: `ask`, `approve`, or `deny`"
        const documented = backticks(row[1]).filter((value) => value !== 'mode').sort();
        assert.deepEqual([...CONFIRMATION_MODES].sort(), documented);
    });

    it('documents exactly the decisions the hub may answer with', () => {
        const response = section(readDocument(), '### Response');
        const match = /`decision` must be (.+?)\./.exec(response);
        assert.ok(match, `the confirmation response decision rule moved. ${PARSER_HINT}`);
        const documented = backticks(match[1]).sort();
        assert.deepEqual(documented, ['approved', 'denied']);
        const request = {
            worker_id: 'w', session_id: 's', run_id: 'r', confirmation_id: 'c',
        };
        for (const decision of documented) {
            assert.equal(buildConfirmationResponse(request, decision).data.decision, decision);
        }
        assert.throws(() => buildConfirmationResponse(request, 'maybe'), /approved or denied/);
    });

    it('sends only the two documented envelope types', () => {
        const markdown = readDocument();
        const match = /`type` must be `payload` or `signal`/.exec(markdown);
        assert.ok(match, `the hub-to-worker envelope rule moved. ${PARSER_HINT}`);
        assert.equal(buildPayload({
            requestId: 'r', content: [{ type: 'text', raw: 'x', modality: 'text' }] }).type, 'payload');
        assert.equal(buildSignal({ operation: 'status' }).type, 'signal');
    });
});
