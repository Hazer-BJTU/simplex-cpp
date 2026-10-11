import assert from 'node:assert/strict';
import { it } from 'node:test';
import { firstEndingAfter, lastStartingBefore, ScrollOwnership } from '../web/src/app/scrollAnchors.ts';

it('finds the first round below an edge through gaps, exact boundaries and empty lists', () => {
    const boxes = [{ top: 10, bottom: 50 }, { top: 70, bottom: 100 }, { top: 100, bottom: 120 }];
    for (let edge = -5; edge <= 135; edge += 1) {
        const expected = boxes.findIndex(box => box.bottom > edge);
        assert.equal(firstEndingAfter(boxes, edge, box => box.bottom), expected < 0 ? boxes.length : expected);
    }
    assert.equal(firstEndingAfter([], 0, () => { throw new Error('empty lookup'); }), 0);
});

it('finds the deepest preceding start with nested and equal-starting candidates', () => {
    const starts = [10, 10, 20, 40, 40, 50];
    for (let edge = -5; edge <= 60; edge += 1) {
        assert.equal(lastStartingBefore(starts, edge, value => value), starts.findLastIndex(value => value <= edge));
    }
    assert.equal(lastStartingBefore([], 0, () => { throw new Error('empty lookup'); }), -1);
});

it('probes logarithmically many current boxes instead of scanning a long transcript', () => {
    const boxes = Array.from({ length: 16384 }, (_, index) => ({ top: index * 20, bottom: index * 20 + 16 }));
    for (const edge of [-1, 8000, 300000, 400000]) {
        let reads = 0;
        firstEndingAfter(boxes, edge, box => { reads += 1; return box.bottom; });
        assert.ok(reads <= 15);
        reads = 0;
        lastStartingBefore(boxes, edge, box => { reads += 1; return box.top; });
        assert.ok(reads <= 15);
    }
});

it('reads fresh geometry when existing members resize or move', () => {
    const boxes = [{ top: 0, bottom: 100 }, { top: 120, bottom: 220 }];
    assert.equal(firstEndingAfter(boxes, 110, box => box.bottom), 1);
    boxes[0].bottom += 40;
    boxes[1].top += 40;
    boxes[1].bottom += 40;
    assert.equal(firstEndingAfter(boxes, 110, box => box.bottom), 0);
    assert.equal(lastStartingBefore(boxes, 130, box => box.top), 0);
});

it('recognizes delayed and repeated programmatic echoes throughout a recent input window', () => {
    const owner = new ScrollOwnership();
    owner.intent(10);
    owner.wrote(300.25);
    for (const now of [11, 50, 999, 2000]) assert.equal(owner.readerMoved(300.25, now), false);
    owner.intent(2100);
    assert.equal(owner.readerMoved(300.25, 2101), false, 'new intent does not turn a pending old echo into input');
    assert.equal(owner.readerMoved(250, 2102), true, 'actual input movement wins over the outstanding write');
});

it('records accepted clamped/fractional targets and bounds unaccompanied scroll intent', () => {
    const owner = new ScrollOwnership();
    owner.intent(0);
    owner.wrote(0);
    assert.equal(owner.readerMoved(0, 500), false);
    assert.equal(owner.readerMoved(10, 1001), false, 'expired input never changes follow state');
    owner.intent(2000);
    assert.equal(owner.readerMoved(10.5, 2001), true);
    owner.wrote(10.5);
    assert.equal(owner.readerMoved(10.5, 2002), false);
    assert.equal(owner.readerMoved(100, 2003), true);
    owner.wrote(100);
    owner.wrote(200);
    assert.equal(owner.readerMoved(200, 2004), false, 'coalesced events use the latest actual target');
});

it('treats layout clamping as automatic while fresh input uses the changed dimensions', () => {
    const owner = new ScrollOwnership();
    const initial = { height: 1000, viewport: 100, width: 500 };
    const enlarged = { ...initial, viewport: 300 };
    owner.wrote(900, initial);
    owner.intent(0, 900, initial);
    assert.equal(owner.readerMoved(780, 1, initial), true);
    assert.equal(owner.readerMoved(700, 2, enlarged), false);
    assert.equal(owner.readerMoved(700, 3, enlarged), false);
    owner.intent(4, 700, enlarged);
    assert.equal(owner.readerMoved(600, 5, enlarged), true);
    const reflowed = { ...enlarged, height: 1300, width: 400 };
    owner.intent(6, 600, reflowed);
    assert.equal(owner.readerMoved(500, 7, reflowed), true);
});

it('explicit following supersedes old keyboard momentum without suppressing fresh input', () => {
    const owner = new ScrollOwnership();
    owner.intent(0);
    assert.equal(owner.readerMoved(500, 1), true);
    owner.resumeFollowing();
    owner.wrote(1000);
    assert.equal(owner.readerMoved(800, 2), false);
    owner.intent(3);
    assert.equal(owner.readerMoved(700, 4), true);
});
