import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import ts from 'typescript';
const output = ts.transpileModule(readFileSync(new URL('../src/explorerTree.ts', import.meta.url), 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.ESNext }
}).outputText;
const { instrumentTree, countInstruments, parentFolders } = await import(`data:text/javascript;base64,${Buffer.from(output).toString('base64')}`);
const files = [
  { path: 'z.flows' }, { path: 'ADCxGather/02-performance/10-ten.flows' },
  { path: 'ADCxGather/01-signals/01-sine.flows' }, { path: 'ADCxGather/02-performance/2-two.flows' },
  { path: 'ADCxGather/01-signals/01-sine.flows' }, { path: '../outside.flows' },
  { path: 'directory', kind: 'directory' }, { path: 'Unicode/音.flows' },
];
const tree = instrumentTree(files);
assert.equal(countInstruments(tree), 5);
assert.deepEqual(tree.map(node => node.name), ['ADCxGather', 'directory', 'Unicode', 'z.flows']);
assert.deepEqual(tree[0].children.map(node => node.name), ['01-signals', '02-performance']);
assert.deepEqual(tree[0].children[1].children.map(node => node.name), ['2-two.flows', '10-ten.flows']);
assert.equal(tree[0].children[0].children[0].path, 'ADCxGather/01-signals/01-sine.flows');
assert.equal(countInstruments(instrumentTree(files, ' PERFORMANCE ')), 2, 'A folder-name search must find its instruments.');
assert.equal(countInstruments(instrumentTree(files, 'sine')), 1);
assert.deepEqual(instrumentTree(files, 'absent'), []);
assert.deepEqual(parentFolders('ADCxGather/01-signals/01-sine.flows'), ['ADCxGather', 'ADCxGather/01-signals']);
assert.deepEqual(parentFolders('a\\b\\c.flows'), ['a', 'a/b']);
assert.deepEqual(parentFolders('root.flows'), []);
console.log('Explorer: hierarchical paths, stable sorting, filtering, deduplication and active ancestors passed.');
