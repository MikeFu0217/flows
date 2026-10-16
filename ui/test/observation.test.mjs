import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import ts from 'typescript';
const output = ts.transpileModule(readFileSync(new URL('../src/observation.ts', import.meta.url), 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.ESNext }
}).outputText;
const { Observation, beatScopeAvailable, captureState, heldLabel } = await import(`data:text/javascript;base64,${Buffer.from(output).toString('base64')}`);
const store = new Observation();
const wave = { name:'raw', shape:'waveform', kind:'audio', valid:true, values:[0,.5,0,-.5], windowReady:true };
const frame = (sequence, fields = {}) => ({ sequence, programRevision:1, generation:1, parameterEpoch:0, observationEpoch:1, signals:[wave], gap:false, voices:1, ...fields });
const beatWave = {...wave,beatAvailable:true,beatPhase:.7,beatMin:[0],beatMax:[.5],beatAge:[0]};
assert.equal(beatScopeAvailable(frame(0,{captureMode:'transport'}),beatWave),true);
assert.equal(beatScopeAvailable(frame(0,{captureMode:'transport',frozen:true}),beatWave),true,'Explicit Hold can display a frozen transport beat');
assert.equal(beatScopeAvailable(frame(0,{captureMode:'transport',captureIdle:true}),beatWave),false,'Idle cannot display a stale transport beat');
assert.equal(beatScopeAvailable(frame(0,{captureMode:'transport'}),wave),false);
assert.equal(beatScopeAvailable(frame(0,{captureMode:'transport'})),false);
for(const captureMode of ['performance','key',undefined]) {
  assert.equal(beatScopeAvailable(frame(0,{captureMode}),beatWave),false,'Manual or unspecified capture must ignore leftover beat metadata');
  assert.equal(beatScopeAvailable(frame(0,{captureMode,frozen:true}),{...beatWave,held:true}),false,'Holding a manual capture cannot create a beat clock');
}
assert.equal(store.accept(frame(1)),true);
assert.equal(store.accept(frame(1)),false);
store.accept(frame(2,{gap:true,status:'capture gap',signals:[]}));
assert.deepEqual(store.frame.signals[0].values,wave.values);
assert.equal(store.frame.signals[0].held,true,'Retained capture must be labelled held');
store.accept(frame(3,{signals:[{...wave,values:[0,0,0,0]}]}));
assert.deepEqual(store.frame.signals[0].values,[0,0,0,0],'Silence is a real valid zero signal');
assert.equal(store.frame.signals[0].held,false);
store.pause(true);
store.accept(frame(4,{signals:[{...wave,values:[1,0,-1,0]}],voices:2}));
assert.deepEqual(store.frame.signals[0].values,[0,0,0,0]);
assert.equal(store.live.voices,2,'Hold must not freeze live audio meters');
assert.equal(store.frame.frozen,true);
store.pause(false);
assert.deepEqual(store.frame.signals[0].values,[1,0,-1,0]);
store.accept(frame(5,{signals:[{name:'raw',shape:'waveform',valid:false,error:'Division by zero in analysis.'}]}));
assert.equal(store.frame.signals[0].valid,false,'Real analysis errors must never look like valid held data');
store.accept(frame(6));store.pause(true);
store.accept(frame(7,{programRevision:2,gap:true,signals:[]}));
assert.equal(store.paused,false);assert.equal(store.frame.signals.length,0,'A changed instrument must not show old capture');
store.reset(4);
assert.equal(store.accept(frame(8,{programRevision:3})),false,'Late old-document telemetry must be rejected');
assert.equal(store.accept(frame(9,{programRevision:4})),true);
const power = {name:'energy',shape:'scalar',kind:'analysis',valid:true,scalar:.125};
store.accept(frame(10,{programRevision:4,signals:[power]}));
store.accept(frame(11,{programRevision:4,signals:[{...power,valid:false,error:'Waiting for 4096 continuous samples.'}]}));
assert.equal(store.frame.signals[0].shape,'scalar');assert.equal(store.frame.signals[0].held,true);
store.accept(frame(12,{programRevision:4,signals:[{...wave,spectrum:[1,.2],spectrumFrequencies:[100,200]}]}));
store.accept(frame(13,{programRevision:4,signals:[{...wave,windowReady:false}]}));
assert.deepEqual(store.frame.signals[0].spectrum,[1,.2]);assert.equal(store.frame.signals[0].spectrumHeld,true);
console.log('Observation: gaps retain labelled frames; silence stays zero; Hold isolates meters; boundaries and errors invalidate safely.');

store.reset();store.accept(frame(20,{captureMode:'key',keyEpoch:1,signals:[{...wave,spectrum:[1,.2]}]}));
store.accept(frame(21,{captureMode:'key',keyEpoch:2,signals:[{...wave,windowReady:false}]}));
assert.equal(store.frame.signals[0].spectrum,undefined,'New keys must not inherit the previous FFT while the new window fills');
store.pause(true);store.accept(frame(22,{captureMode:'key',keyEpoch:3}));
assert.equal(store.paused,false,'A new key must replace the held previous capture');

store.reset();store.accept(frame(30,{captureMode:'key',keyEpoch:1,signals:[{...wave,spectrum:[1,.2],spectrumFrequencies:[100,200]}]}));
store.accept(frame(31,{captureMode:'key',keyEpoch:1,captureHeld:true,signals:[{...wave,values:[0,0],spectrum:[.1,.3],spectrumFrequencies:[100,200]}]}));
assert.deepEqual(store.frame.signals[0].spectrum,[1,.3],'Short-note preview must retain this key’s spectral peak');
assert.deepEqual(store.frame.signals[0].values,[0,0],'Peak presentation must not rewrite measured waveforms');
assert.deepEqual(store.live.signals[0].spectrum,[.1,.3],'Peak presentation must not rewrite the live measurement');

store.pause(true);
store.accept(frame(32,{captureMode:'key',captureIdle:true,keyEpoch:1,signals:[{...wave,spectrum:[1,.2]}]}));
assert.equal(store.paused,false,'Stopped/idle capture must clear shared Hold');
assert.deepEqual(store.frame.signals,[],'Idle must discard even stale queued spectra');
store.accept(frame(33,{captureMode:'key',captureIdle:true,keyEpoch:1,gap:true,signals:[]}));
assert.deepEqual(store.frame.signals,[],'Gap fallback must not resurrect stopped transport history');
assert.equal(captureState(store.frame,wave),'idle');
store.accept(frame(34,{captureMode:'key',captureIdle:false,keyEpoch:2,signals:[wave]}));
assert.equal(captureState(store.frame,store.frame.signals[0]),'live');
store.accept(frame(35,{captureMode:'key',captureIdle:false,captureHeld:true,keyEpoch:2,signals:[wave]}));
assert.equal(captureState(store.frame,store.frame.signals[0]),'held','All preview surfaces must recognize automatic key hold');
assert.equal(heldLabel(store.frame),'LAST KEY');
assert.equal(captureState(store.frame,undefined),'waiting','A missing capture must not be labelled held');
assert.equal(captureState(store.frame,wave,true),'waiting','A missing spectrum must not be labelled held');
store.pause(true);assert.equal(heldLabel(store.frame),'HELD');
store.accept(frame(36,{captureMode:'transport',captureIdle:false,captureHeld:false,keyEpoch:2,signals:[wave]}));
assert.equal(captureState(store.frame,store.frame.signals[0]),'live');
assert.equal(store.paused,false,'Transport restart must release key hold');

// Manual performance follows measured sound through sustain and release; it
// must not accumulate note peaks or freeze at the former one-beat boundary.
const performanceFrame = (sequence, fields = {}) => frame(sequence, {
  captureMode:'performance', captureIdle:false, captureHeld:false, keyEpoch:1,
  sampleRate:48000, captureStart:sequence*2400, captureEnd:(sequence+1)*2400,
  ...fields,
});
store.reset();
for (let sequence=40; sequence<100; sequence++) {
  const amplitude = sequence<60 ? .8 : sequence<80 ? .3 : .3 * (99-sequence)/19;
  const measured = {...wave,values:[0,amplitude,0,-amplitude],spectrum:[amplitude,.1*amplitude],spectrumFrequencies:[100,200]};
  store.accept(performanceFrame(sequence,{signals:[measured]}));
  assert.deepEqual(store.frame.signals[0].spectrum,measured.spectrum,'Sustain and release spectra must follow the current measured level');
  assert.deepEqual(store.frame.signals[0].values,measured.values);
  assert.equal(store.frame.signals[0].spectrumPeak,undefined,'Live performance cannot preserve an earlier attack peak');
  assert.equal(captureState(store.frame,store.frame.signals[0],true),'live','Long held notes and release tails stay live');
}
assert.deepEqual(store.frame.signals[0].spectrum,[0,0],'The release reaches measured silence');

store.pause(true);
store.accept(performanceFrame(100,{keyEpoch:2,signals:[{...wave,spectrum:[.7,.2]}]}));
assert.equal(store.paused,true,'An overlapping key cannot release an explicitly held display');
assert.deepEqual(store.frame.signals[0].spectrum,[0,0]);
assert.deepEqual(store.live.signals[0].spectrum,[.7,.2],'Live measurement continues behind Hold');
assert.equal(heldLabel(store.frame),'HELD');
store.pause(false);
assert.deepEqual(store.frame.signals[0].spectrum,[.7,.2]);

store.pause(true);
store.accept(performanceFrame(101,{keyEpoch:2,captureIdle:true,signals:[]}));
assert.equal(store.paused,true,'Natural silence preserves a user-requested Hold');
assert.equal(captureState(store.frame,store.frame.signals[0],true),'held');
assert.equal(store.live.captureIdle,true,'Hold must not freeze live performance status');
store.pause(false);
assert.deepEqual(store.frame.signals,[],'Resume after release shows silence, never the old attack');
assert.equal(captureState(store.frame),'idle');
store.accept(performanceFrame(102,{keyEpoch:2,captureIdle:true,gap:true,signals:[]}));
assert.deepEqual(store.frame.signals,[],'Idle capture gaps cannot resurrect a released note');
store.accept(performanceFrame(103,{keyEpoch:3,signals:[{...wave,windowReady:false}]}));
assert.equal(store.frame.signals[0].spectrum,undefined,'A performance starting from silence has no stale spectrum');
store.accept(performanceFrame(104,{keyEpoch:3,signals:[{...wave,spectrum:[.6,.1]}]}));
assert.equal(captureState(store.frame,store.frame.signals[0],true),'live');
store.pause(true);
store.accept(performanceFrame(105,{keyEpoch:3,programRevision:2,captureIdle:true,signals:[]}));
assert.equal(store.paused,false,'An instrument change clears Hold even when the replacement is idle');
assert.deepEqual(store.frame.signals,[]);

store.accept(frame(106,{programRevision:2,captureMode:'transport',signals:[{...wave,spectrum:[.6,.1]}]}));
store.pause(true);
store.accept(performanceFrame(107,{programRevision:2,captureIdle:true,signals:[]}));
assert.equal(store.paused,false,'Stopping the sequencer clears its held transport history');
assert.deepEqual(store.frame.signals,[]);
console.log('Performance: long sustain and ADSR release stay live, spectra follow falling levels, explicit Hold survives new keys and natural silence, stopped transport clears.');

store.reset();
store.accept(frame(120,{captureMode:'transport',signals:[beatWave]}));
store.accept(frame(121,{captureMode:'performance',gap:true,status:'transition',signals:[]}));
assert.deepEqual(store.frame.signals,[],'Leaving transport must discard its cached beat waveform');
const pitch={...wave,name:'key.pitch',kind:'control',values:[60,60,60,60],...{beatAvailable:true,beatPhase:.9}};
const envelope={...wave,name:'amp',kind:'control',values:[.7,.5,.3,.1]};
store.accept(frame(122,{captureMode:'performance',signals:[pitch,envelope]}));
assert.equal(beatScopeAvailable(store.frame,store.frame.signals[0]),false,'Manual pitch cannot sweep even if a stale producer sends beat metadata');
assert.deepEqual(store.frame.signals[0].values,[60,60,60,60],'Pitch remains an event-held value during release');
assert.deepEqual(store.frame.signals[1].values,[.7,.5,.3,.1],'Release envelope values remain measured data');
assert.equal(captureState(store.frame,store.frame.signals[1]),'live','Removing the beat clock cannot freeze the release');
console.log('Scope clock: only transport enables beat display; manual/Hold/stale beat metadata use fixed-span waveforms without changing pitch or ADSR values.');
