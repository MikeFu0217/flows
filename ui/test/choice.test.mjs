import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import ts from 'typescript';
const output = ts.transpileModule(readFileSync(new URL('../src/choice.ts', import.meta.url), 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.ESNext }
}).outputText;
const { applyChoice } = await import(`data:text/javascript;base64,${Buffer.from(output).toString('base64')}`);

// A menu edit must preserve UTF-16 source around it, including comments and
// another call using the same symbol, and must reject stale/disabled targets.
const source = '# 音色 🎹\na = filt(x, mode: lowpass) # lowpass\nb = filt(y, mode: lowpass)\n';
const from = source.indexOf('lowpass');
const choice = { id: 'a.mode', name: 'mode', value: 'lowpass', from, to: from + 7, implicit: false, prefix: '', suffix: '',
  options: [{ value: 'lowpass', enabled: true }, { value: 'highpass', enabled: true }, { value: 'bandpass', enabled: false, reason: 'Unavailable' }] };
assert.equal(applyChoice(source, choice, 'highpass'), source.slice(0, from) + 'highpass' + source.slice(from + 7));
assert.equal(applyChoice(source, choice, 'lowpass'), null);
assert.equal(applyChoice(source, choice, 'bandpass'), null);
assert.equal(applyChoice(source, choice, 'sine'), null);
assert.equal(applyChoice('# shift\n' + source, choice, 'highpass'), null);
assert.equal(applyChoice(source.replace('mode: lowpass', 'mode: variable'), choice, 'highpass'), null);
for (const bounds of [{ from: -1 }, { to: source.length + 1 }, { from: from + .5 }, { to: from - 1 }])
  assert.equal(applyChoice(source, { ...choice, ...bounds }, 'highpass'), null);
const call = 'x = lfo(\n    rate: 2hz, # pace\n)\n';
const at = call.lastIndexOf(')');
const implicit = { ...choice, name: 'shape', value: 'sine', from: at, to: at, implicit: true, prefix: '    shape: ', suffix: ',\n', options: [{ value: 'triangle', enabled: true }] };
assert.equal(applyChoice(call, implicit, 'triangle'), 'x = lfo(\n    rate: 2hz, # pace\n    shape: triangle,\n)\n');
assert.equal(applyChoice(call, { ...implicit, to: at + 1 }, 'triangle'), null);
console.log('Choices: precise UTF-16 source replacement; stale/disabled/unknown selection rejection; implicit argument insertion.');
