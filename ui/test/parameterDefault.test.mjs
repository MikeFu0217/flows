import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import ts from 'typescript';

const output = ts.transpileModule(readFileSync(new URL('../src/parameterDefault.ts', import.meta.url), 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.ESNext }
}).outputText;
const { parameterDefaultLiteral } = await import(`data:text/javascript;base64,${Buffer.from(output).toString('base64')}`);

// Reset must reproduce the declared semantic value, even when it lies between
// drag steps or needs more digits than the display. The compiler owns ranges.
for (const value of [-18.25, 0.12589254117941673, 1e-310, Number.MIN_VALUE, Number.MAX_VALUE, 0]) {
  const literal = parameterDefaultLiteral({ hasDefault: true, defaultValue: value, step: 1, hasRange: true, min: -60, max: 0 });
  assert.notEqual(literal, null);
  assert.equal(Number(literal), value, `Reset changed ${value}`);
}
assert.equal(parameterDefaultLiteral({ hasDefault: true, defaultValue: -18.25, step: 1 }), '-18.25');
assert.equal(parameterDefaultLiteral({ hasDefault: false, defaultValue: 0 }), null);
for (const value of [undefined, NaN, Infinity, -Infinity])
  assert.equal(parameterDefaultLiteral({ hasDefault: true, defaultValue: value }), null);
console.log('Parameter defaults: exact round trip independent of drag steps; absent/nonfinite defaults cannot reset.');
