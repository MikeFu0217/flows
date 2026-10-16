import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import ts from 'typescript';
const output = ts.transpileModule(readFileSync(new URL('../src/documents.ts', import.meta.url), 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.ESNext }
}).outputText;
const { DocumentSession, openTab, closeTab, reconcileGroups } = await import(`data:text/javascript;base64,${Buffer.from(output).toString('base64')}`);
const tick = () => new Promise(resolve => setTimeout(resolve, 0));
function harness() {
  let revision = 2, active = 'a.flows';
  const docs = new Map(['a.flows','b.flows'].map((id, i) => [id, { id, path:id, text:id, revision:i+1, savedRevision:i+1, dirty:false, compile:{status:'ready',revision:i+1} }]));
  const log = [], waits = [];
  const snapshot = () => structuredClone({ sessionId:'session', document: docs.get(active) ?? {id:'',path:'',text:'',revision}, documents:[...docs.values()], compile:docs.get(active)?.compile ?? {status:'idle'}, files:[], audio:{}, ai:{} });
  // The active document has the newest session revision, as in the native host.
  docs.get(active).revision = ++revision;
  const session = new DocumentSession(async (method, params = {}) => {
    revision=Math.max(revision,...[...docs.values()].map(doc=>doc.revision));
    log.push({method,...params});
    const wait = waits.find(value => value.method === method && !value.used); if (wait) { wait.used=true; await wait.promise; }
    if (method === 'document.activate') {
      assert.ok(docs.has(params.documentId)); active=params.documentId; docs.get(active).revision=++revision;
    } else if (method === 'document.apply') {
      assert.equal(params.documentId,active,'An edit must never reach another instrument');
      assert.equal(params.baseRevision,docs.get(active).revision,'Edits use the acknowledged revision');
      Object.assign(docs.get(active),{text:params.text,revision:++revision,dirty:true});
    } else if (method === 'document.close') {
      const doc=docs.get(params.documentId); assert.ok(doc);
      if(doc.dirty && !params.save && !params.discard) throw new Error('Unsaved source');
      docs.delete(params.documentId);
      if(active===params.documentId) {active=[...docs.keys()][0] ?? ''; ++revision;if(active)docs.get(active).revision=revision;}
    } else if (method === 'document.save') {
      const doc=docs.get(params.documentId);doc.dirty=false;doc.savedRevision=doc.revision;
    }
    const result=snapshot(); session.receive(result); return result;
  },()=>{});
  session.receive(snapshot());
  const pause = method => { let resume; const promise=new Promise(resolve=>resume=resolve);waits.push({method,promise});return resume; };
  return {session,docs,log,pause,snapshot};
}
{
  const h=harness();h.session.change('a.flows','draft A');
  const resume=h.pause('document.activate'), switching=h.session.activate('b.flows');await tick();
  assert.equal(h.docs.get('a.flows').text,'draft A');
  h.session.change('b.flows','first keystroke B');
  resume();await switching;
  assert.equal(h.docs.get('b.flows').text,'first keystroke B');
  assert.deepEqual(h.log.map(value=>[value.method,value.documentId]),[['document.apply','a.flows'],['document.activate','b.flows'],['document.apply','b.flows']]);
}
{
  const h=harness(),resume=h.pause('document.activate');
  const first=h.session.activate('b.flows');await tick();h.session.change('b.flows','B typed while loading');
  const second=h.session.activate('a.flows');h.session.change('a.flows','A typed while returning');resume();await Promise.all([first,second]);
  assert.equal(h.session.snapshot.document.id,'a.flows');
  assert.equal(h.docs.get('a.flows').text,'A typed while returning');assert.equal(h.docs.get('b.flows').text,'B typed while loading');
  assert.equal(h.session.drafts.size,0);
}
{
  const h=harness(),resume=h.pause('document.apply');h.session.change('a.flows','one');
  const flushing=h.session.flush();await tick();h.session.change('a.flows','two');resume();await flushing;
  assert.equal(h.docs.get('a.flows').text,'two');assert.equal(h.session.drafts.size,0);
  assert.equal(h.log.filter(value=>value.method==='document.apply').length,2);
}
{
  const h=harness();h.session.change('a.flows','local');
  h.docs.get('a.flows').text='external';h.docs.get('a.flows').revision=9;h.session.receive(h.snapshot());
  await assert.rejects(h.session.activate('b.flows'),/draft is preserved/);assert.equal(h.session.text('a.flows'),'local');
  assert.equal(h.session.snapshot.document.id,'a.flows');
  h.session.reload('a.flows');await h.session.activate('b.flows');assert.equal(h.session.snapshot.document.id,'b.flows');
}
{
  const h=harness();h.session.change('b.flows','unsent discarded source');await h.session.close('b.flows','discard');
  assert.equal(h.session.snapshot.document.id,'a.flows');assert.equal(h.session.drafts.size,0);
  assert.ok(!h.log.some(value=>value.method==='document.apply'));
  h.session.change('a.flows','save this');await h.session.close('a.flows','save');assert.equal(h.session.snapshot.document.id,'');
  assert.equal(h.log.at(-2).text,'save this');assert.equal(h.log.at(-1).save,true);
}
{
  const h=harness();await h.session.close('b.flows');
  assert.equal(h.session.snapshot.document.id,'a.flows');assert.deepEqual(h.log.map(value=>value.method),['document.close']);
}
{
  const h=harness();h.docs.get('b.flows').dirty=true;h.session.receive(h.snapshot());await h.session.close('b.flows','save');
  assert.equal(h.session.snapshot.document.id,'a.flows');assert.deepEqual(h.log.map(value=>value.method),['document.close']);
}
{
  const h=harness();const session=new DocumentSession(async()=>{throw new Error('Disk unavailable');},()=>{});
  session.receive(h.snapshot());session.change('b.flows','keep rejected discard');
  await assert.rejects(session.close('b.flows','discard'),/Disk unavailable/);
  assert.equal(session.text('b.flows'),'keep rejected discard');
}
{
  let groups=[{tabs:['a'],active:'a'},{tabs:[],active:''}];
  groups=openTab(groups,1,'a');groups=openTab(groups,1,'b');groups=closeTab(groups,1,'a');
  assert.deepEqual(groups,[{tabs:['a'],active:'a'},{tabs:['b'],active:'b'}]);
  groups=closeTab(groups,1,'b');assert.deepEqual(groups[1],{tabs:[],active:''});
  assert.deepEqual(reconcileGroups([{tabs:['closed','a'],active:'closed'},{tabs:['b'],active:'b'}],['a','b','c'],'b'),[{tabs:['a','c'],active:'a'},{tabs:['b'],active:'b'}]);
}
console.log('Document session: outgoing flush, typing during activation, rapid focus changes, in-flight edits, conflict preservation, save/discard close and independent tab groups.');
