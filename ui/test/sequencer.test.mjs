import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import ts from 'typescript';
const output = ts.transpileModule(readFileSync(new URL('../src/sequencer.ts', import.meta.url), 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.ESNext }
}).outputText;
const { sequenceNote, withLaneSound } = await import(`data:text/javascript;base64,${Buffer.from(output).toString('base64')}`);
const pattern = { root: 0, transpose: 0, scale: 'major' };
// C major: equally close chromatic notes move downward, including black keys.
assert.deepEqual(Array.from({ length: 12 }, (_, i) => sequenceNote(48 + i, pattern)), [48, 48, 50, 50, 52, 53, 53, 55, 55, 57, 57, 59]);
assert.deepEqual(Array.from({ length: 12 }, (_, i) => sequenceNote(48 + i, { ...pattern, scale: 'pentatonic' })), [48, 48, 50, 50, 52, 52, 55, 55, 55, 57, 57, 60]);
assert.equal(sequenceNote(49, { ...pattern, scale: 'minor' }), 48);
assert.equal(sequenceNote(0, { ...pattern, root: 11 }), 1);
assert.equal(sequenceNote(127, { ...pattern, root: 11 }), 126);
assert.equal(sequenceNote(0, { ...pattern, transpose: -24, scale: 'chromatic' }), 0);
assert.equal(sequenceNote(127, { ...pattern, transpose: 24, scale: 'chromatic' }), 127);
assert.equal(sequenceNote(36, { ...pattern, transpose: 13 }), 48);
console.log('Sequencer pitch labels: scale roots, major pentatonic, downward ties, transpose and MIDI endpoints.');

const rows = [{ note: 30, mute: false, steps: [] }, { note: 37, mute: false, steps: [] }];
const sampled = withLaneSound({ rows, sound: { kind: 'active' } }, 0, { kind: 'sample', path: 'Samples/kick.wav', playback: 'one_shot' });
assert.equal(sampled.rows[0].note, 48, 'A new sample must start at its original pitch, not the bass note');
assert.equal(sampled.rows[1].note, 37);
assert.equal(withLaneSound({ ...sampled, rows: [{ ...sampled.rows[0], note: 50 }, rows[1]] }, 0, { kind: 'sample', path: 'Samples/kick.wav', playback: 'gated' }).rows[0].note, 50, 'Changing playback mode must preserve an intentional pitch');
