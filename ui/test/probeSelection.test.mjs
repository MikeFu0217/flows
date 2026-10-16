import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import ts from 'typescript';
const output = ts.transpileModule(readFileSync(new URL('../src/probeSelection.ts', import.meta.url), 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.ESNext }
}).outputText;
const { planObservation, frameForObservationVoice, subscribeObservation } = await import(`data:text/javascript;base64,${Buffer.from(output).toString('base64')}`);
const signal = (name, captureRoots, kind = 'analysis') => ({ name, captureRoots, kind, from: 0, to: 1 });

// The public probe example has more than eight visible names but only three
// captured inputs. Every view must remain available together.
const views = Array.from({ length: 10 }, (_, index) => signal(`view${index}`, [index % 3]));
const shared = planObservation([...views, signal('raw', [0], 'audio'), signal('filtered', [1], 'audio'), signal('amp', [2], 'control')]);
assert.equal(shared.roots, 3);
assert.equal(shared.signals.length, 13);
assert.deepEqual(shared.skipped, {});

// Auto-observed DSP channels cannot push a valid eight-input analysis over
// budget, and focusing another signal reallocates capacity deterministically.
const all = [...Array.from({ length: 9 }, (_, index) => signal(`raw${index}`, [index], 'audio')), signal('diff', [0, 1, 2, 3, 4, 5, 6, 7])];
const analysis = planObservation(all);
assert.equal(analysis.roots, 8);
assert.ok(analysis.signals.includes('diff'));
assert.ok(!analysis.signals.includes('raw8'));
assert.match(analysis.skipped.raw8, /Select this signal/);
const focused = planObservation(all, ['unknown', 'raw8', 'raw8']);
assert.ok(focused.signals.includes('raw8'));
assert.equal(focused.roots, 8);
assert.match(focused.skipped.diff, /budget/);
assert.deepEqual(planObservation(all, ['raw8']).signals, focused.signals);

const tooLarge = planObservation([signal('oversized', [0, 1, 2, 3, 4, 5, 6, 7, 8]), signal('small', [0])], ['oversized']);
assert.deepEqual(tooLarge.signals, ['small']);
assert.match(tooLarge.skipped.oversized, /needs 9 captured inputs/);
assert.deepEqual(planObservation([signal('constant', [])]).signals, ['constant']);
assert.equal(planObservation([signal('a', [1, 1]), signal('alias', [1])]).roots, 1);
const many = planObservation(Array.from({ length: 129 }, (_, index) => signal(`view${index}`, [0])), ['view128']);
assert.equal(many.signals.length, 128);
assert.ok(many.signals.includes('view128'));
assert.match(many.skipped.view127, /128-view limit/);

const frame = { sequence: 10, programRevision: 7, captureVoice: -1, observationEpoch: 8, signals: [{ name: 'amp', values: [1] }], outputPeak: .75, cpu: 2.5, voices: 2 };
assert.equal(frameForObservationVoice(frame, -1), frame);
const outgoing = frameForObservationVoice(frame, 0);
assert.deepEqual(outgoing.signals, []);
assert.equal(outgoing.observationEpoch, -1);
assert.equal(outgoing.outputPeak, .75);
assert.equal(outgoing.cpu, 2.5);
assert.equal(outgoing.voices, 2);
assert.equal(outgoing.programRevision, 7);
const selected = { ...frame, captureVoice: 0, observationEpoch: 9 };
assert.equal(frameForObservationVoice(selected, 0), selected);
assert.deepEqual(frame.signals, [{ name: 'amp', values: [1] }], 'Filtering must not mutate incoming capture data');
// Sharing a capture root does not remove the analyzer's transform-work budget.
// A large automatic group must fall back once to the inspected view, then allow
// another view to be selected; a genuinely invalid single view must still fail.
const manySpectra = Array.from({ length: 33 }, (_, index) => `spec${index}`), calls = [];
const send = async names => {
  calls.push(names);
  if (names.length > 32) throw new Error('Analysis budget exceeded: at most 32 spectral transforms');
  return { observed: names };
};
const reduced = await subscribeObservation(send, manySpectra, 'spec20');
assert.deepEqual(calls, [manySpectra, ['spec20']]);
assert.deepEqual(reduced.signals, ['spec20']);
assert.match(reduced.reduced, /32 spectral transforms/);
const other = await subscribeObservation(send, manySpectra, 'spec21');
assert.deepEqual(other.signals, ['spec21']);
assert.deepEqual(calls.slice(2), [manySpectra, ['spec21']]);
let failedCalls = 0;
await assert.rejects(subscribeObservation(async () => { ++failedCalls; throw new Error('Each view may depend on at most 128 nodes'); }, manySpectra, 'tooLarge'), /128 nodes/);
assert.equal(failedCalls, 2, 'A failed fallback must not retry indefinitely');
failedCalls = 0;
await assert.rejects(subscribeObservation(async () => { ++failedCalls; throw new Error('Single view is over budget'); }, ['tooLarge'], 'tooLarge'), /Single view/);
assert.equal(failedCalls, 1, 'Do not retry an already individual request');
console.log('Probe selection: shared roots, priority, budget errors and voice transitions preserve correct displays and live meters.');

const stereoFrame = { ...frame, captureProjection: 'left' };
assert.equal(frameForObservationVoice(stereoFrame, -1, 'left'), stereoFrame);
const oldChannel = frameForObservationVoice(stereoFrame, -1, 'right');
assert.deepEqual(oldChannel.signals, []);
assert.equal(oldChannel.outputPeak, .75);
assert.equal(oldChannel.observationEpoch, -1);
