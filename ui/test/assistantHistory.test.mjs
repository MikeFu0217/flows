import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { transform } from 'esbuild';
const source = await readFile(new URL('../src/assistantHistory.ts', import.meta.url), 'utf8');
const output = await transform(source, { loader: 'ts', format: 'esm' });
const { historyRows, mergeAgentEvent, mergeLoadedMessages } = await import('data:text/javascript;base64,' + Buffer.from(output.code).toString('base64'));
const message = { id: 'request-a', prompt: 'Explain', text: '', status: 'working', documentId: 'bass.flows', baseRevision: 3 };
const done = mergeAgentEvent([message], { requestId: message.id, status: 'done', text: '# Bass', proposalAvailable: true });
assert.equal(done[0].text, '# Bass');
assert.deepEqual(mergeAgentEvent(done, { requestId: message.id, status: 'working', text: 'Running tool...' }), done);
assert.deepEqual(mergeAgentEvent(done, { requestId: 'other', status: 'done', text: 'Other response' }), done);
assert.deepEqual(mergeLoadedMessages([message], done), done);
assert.equal(mergeLoadedMessages([{ ...done[0], proposalAvailable: false }], done)[0].proposalAvailable, false);
const rows = [
  { id: 'a', title: 'Bass', archived: false, deleted: false, updatedAt: 1 },
  { id: 'b', title: 'Filter', archived: false, deleted: false, updatedAt: 2 },
  { id: 'c', title: 'Old bass', archived: true, deleted: false, updatedAt: 3 },
  { id: 'd', title: 'Deleted', archived: true, deleted: true, updatedAt: 4 },
];
assert.deepEqual(historyRows(rows, 'active').map(row => row.id), ['b', 'a']);
assert.deepEqual(historyRows(rows, 'archived').map(row => row.id), ['c']);
assert.deepEqual(historyRows(rows, 'trash').map(row => row.id), ['d']);
assert.deepEqual(historyRows(rows, 'active', 'BASS').map(row => row.id), ['a']);
console.log('Assistant history UI: folders/search, request identity, terminal-before-ack ordering, late working hydration and persisted proposal flags passed.');

const think = { id: 'thinking-0', kind: 'thinking', label: 'Thinking', detail: 'Waiting for the model', state: 'running' };
let live = mergeAgentEvent([{ ...message, text: 'Existing reply', startedAt: 100 }], { requestId: message.id, status: 'working', activity: think });
assert.equal(live[0].text, 'Existing reply', 'Progress must not replace the reply');
assert.equal(live[0].activity[0].state, 'running');
live = mergeAgentEvent(live, { requestId: message.id, status: 'working', activity: { ...think, state: 'done', durationMs: 40 } });
live = mergeAgentEvent(live, { requestId: message.id, status: 'working', activity: think });
assert.equal(live[0].activity.length, 1, 'Updates must replace a step rather than duplicate it');
assert.equal(live[0].activity[0].state, 'done', 'Late start must not revive a completed spinner');
const tool = { id: 'tool-0', kind: 'tool', label: 'Reading source', detail: 'bass.flows', state: 'running' };
live = mergeAgentEvent(live, { requestId: message.id, status: 'working', activity: tool });
assert.deepEqual(mergeLoadedMessages([{ ...message, text: '' }], live), live, 'A list refresh must preserve live progress');
const cancelled = mergeAgentEvent(live, { requestId: message.id, status: 'cancelled', text: 'Stopped' });
assert.equal(cancelled[0].activity[1].state, 'cancelled', 'Cancel must stop tool animations');
const failed = mergeAgentEvent(live, { requestId: message.id, status: 'failed', error: 'Endpoint failed' });
assert.equal(failed[0].activity[1].state, 'failed');
const completed = mergeAgentEvent(live, { requestId: message.id, status: 'done', text: 'Final answer', proposalAvailable: true });
assert.equal(completed[0].text, 'Final answer');
assert.equal(completed[0].activity[1].state, 'done');
assert.deepEqual(mergeAgentEvent(completed, { requestId: message.id, status: 'working', activity: tool }), completed);
const hydrated = mergeLoadedMessages([{ ...completed[0], text: 'Saved answer', proposalAvailable: false, activity: undefined }], completed);
assert.equal(hydrated[0].text, 'Saved answer');
assert.equal(hydrated[0].proposalAvailable, false);
assert.equal(hydrated[0].activity.length, 2, 'Hydration must preserve the local activity trace');
for (let i = 0; i < 50; i++) live = mergeAgentEvent(live, { requestId: message.id, status: 'working', activity: { ...tool, id: 'tool-' + i } });
assert.equal(live[0].activity.length, 24, 'Activity trace must stay bounded');
console.log('Assistant activity: separate answer/progress, ordered step updates, cancellation/failure, terminal races, hydration and bounded traces passed.');

const terminalRefresh = mergeLoadedMessages([{ ...message, status: 'done', text: 'Saved answer' }], live);
assert.ok(terminalRefresh[0].activity.every(step => step.state !== 'running'), 'Saved terminal status must stop spinners even before the final event arrives');

let lateActivity = [{ ...message, status: 'done', text: 'Saved answer' }];
for (const [id, state] of [['thinking-0', 'done'], ['tool-0', 'done'], ['tool-1', 'failed']]) {
  lateActivity = mergeAgentEvent(lateActivity, { requestId: message.id, status: 'done', activity: { ...tool, id, state } });
}
assert.equal(lateActivity[0].activity.length, 3, 'Terminal activity must merge into an already saved answer');
assert.equal(lateActivity[0].activity[2].state, 'failed');
assert.equal(lateActivity[0].text, 'Saved answer');
