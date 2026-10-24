import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import ts from 'typescript';

const output = ts.transpileModule(readFileSync(new URL('../src/flowLayout.ts', import.meta.url), 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.ESNext }
}).outputText;
const { layoutFlow, visibleFlowGraph } = await import(`data:text/javascript;base64,${Buffer.from(output).toString('base64')}`);
const node = (id, fields = {}) => ({ id, label: id, signal: id, op: 'lfo', kind: 'control', from: 0, to: 1, parameterIds: [], choiceIds: [], ...fields });
const choice = (id, value = 'triangle') => ({ id, name: id, value, from: 0, to: 0, implicit: true, prefix: '', suffix: '', options: [] });
const parameters = new Map([['rate', { id: 'rate', value: 1 }]]);
const choices = new Map(['shape', 'mode', 'bipolar'].map(id => [id, choice(id)]));
const graph = {
  nodes: [node('motion', { parameterIds: ['rate'], choiceIds: ['shape', 'mode', 'bipolar', 'stale'] }), node('amp')],
  edges: [], output: 'amp'
};
const baseline = layoutFlow(graph, parameters), expanded = layoutFlow(graph, parameters, choices);
const motion = expanded.nodes.find(node => node.id === 'motion');
const baseMotion = baseline.nodes.find(node => node.id === 'motion');
assert.ok(motion.height >= baseMotion.height + 3 * 26, 'Three dropdown hit areas must fit without stealing knob space.');
for (const a of expanded.nodes) {
  assert.ok(a.x >= 0 && a.y >= 0 && a.x + a.width <= expanded.width && a.y + a.height <= expanded.height);
  for (const b of expanded.nodes) if (a !== b && a.x === b.x)
    assert.ok(a.y + a.height <= b.y || b.y + b.height <= a.y, 'Choice rows must not overlap the next node.');
}
const changed = layoutFlow(graph, new Map([['rate', { id: 'rate', value: 20 }]]),
  new Map([...choices].map(([id]) => [id, choice(id, 'sample_hold')])));
assert.deepEqual(changed, expanded, 'Changing a value or its text length must not move nodes.');

const inputGraph = { nodes: [node('key.pitch', { choiceIds: ['mode'] })], edges: [], output: 'key.pitch' };
assert.ok(layoutFlow(inputGraph, new Map(), choices).nodes[0].height > layoutFlow(inputGraph, new Map()).nodes[0].height,
  'An event node with a policy dropdown must leave its compact layout.');
assert.deepEqual(layoutFlow(graph, parameters, new Map()), baseline, 'Legacy graphs need no choice metadata.');
console.log('Flow layout: dropdown rows preserve knob space, avoid overlap and stay stable across value edits.');

// An unused key input should not occupy space; an input feeding a probe stays.
const filtered = visibleFlowGraph({nodes:[node('key.pitch'),node('key.gate'),node('key.velocity'),node('probe'),node('out')], edges:[{source:'key.pitch',target:'out'}, {source:'key.velocity',target:'probe'}],output:'out'});
assert.deepEqual(filtered.nodes.map(n=>n.id),['key.pitch','key.velocity','probe','out']);
assert.equal(filtered.edges.length,2);

// Interface text scaling must reserve enough room for every card and choice row.
for (const scale of [.625, 1.0625, 1.5, 2]) {
  const scaled = layoutFlow(graph, parameters, choices, scale);
  assert.deepEqual(scaled.edges, expanded.edges);
  assert.equal(scaled.nodes.length, expanded.nodes.length);
  for (let i = 0; i < scaled.nodes.length; i++) {
    const n = scaled.nodes[i], original = expanded.nodes[i];
    for (const metric of ['x', 'y', 'width', 'height']) assert.equal(n[metric], original[metric] * scale);
    assert.ok(n.x + n.width <= scaled.width + .001 && n.y + n.height <= scaled.height + .001);
  }
}
