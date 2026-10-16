import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import ts from 'typescript';
const output = ts.transpileModule(readFileSync(new URL('../src/parameterDrag.ts', import.meta.url), 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.ESNext }
}).outputText;
const { parameterDrag, parameterMapping, parameterTravel, parameterInput, normaliseParameter, parameterStep, projectParameters } = await import(`data:text/javascript;base64,${Buffer.from(output).toString('base64')}`);
const p = { id:'level', from:2, to:5, value:.5, hasRange:true, min:0, max:1, step:.001 };
for (const parameter of [p, {...p, min:20, max:20000, value:440, step:1, scale:'log'}, {...p,min:-120,max:24,value:-18,step:.1}, {...p,min:1,max:8,value:3,step:1,discrete:true}]) {
  const drag = parameterDrag(parameter);
  assert.equal(drag(0), parameter.value);
  let previous=parameter.value;
  for(let i=0;i<Math.ceil(parameterTravel(parameter))+1;i++) {const next=drag(1); assert(next>=previous && next<=parameter.max); previous=next;}
  assert.equal(previous,parameter.max);
  assert(drag(-40)<parameter.max, 'Reversal must respond without traversing overshoot');
  assert.equal(drag(-1000),parameter.min);
  assert(drag(30)>parameter.min || parameter.discrete, 'Lower endpoint must not accumulate overshoot');
  assert.equal(drag(1000),parameter.max);
}
const low=parameterDrag({...p,value:.01})(1)-.01, mid=parameterDrag(p)(1)-.5;
assert(Math.abs(mid-low)<.0011,'Linear controls must have position-independent response');
const normal=parameterDrag(p)(5)-.5, fine=parameterDrag(p)(5,true)-.5;
assert(normal>fine*8 && normal<fine*12,'Shift should be about ten times finer');
assert.equal(parameterStep({...p,step:.01},true),.001);
assert.equal(parameterStep({...p,step:1,discrete:true},true),1);
assert.equal(parameterStep({...p,step:1.5,discrete:true},true),2);
const discrete=parameterDrag({...p,min:1,max:8,value:3,step:1,discrete:true});
for(let i=0;i<1000;i++) assert(Number.isInteger(discrete(.4,true)));
const continuous=parameterDrag({...p,value:.51725});
assert.equal(continuous(0,true),.51725,'Modifier changes without motion must not quantize');
const value=continuous(5); assert.equal(continuous(0,true),value);
assert.equal(normaliseParameter(.99,{...p,step:.3}),.9);
assert.equal(normaliseParameter(2,{...p,step:.3}),1);
const relative=parameterDrag({...p,hasRange:false,value:-2}); assert(relative(10)>-2);
assert.equal(parameterDrag({...p,min:1,max:1,value:1})(100),1);
// Musical operating regions, source-unit invariance and exact input.
const time = {...p,name:'A',unit:'seconds',value:1,min:0,max:30000,step:.01,response:'offset_log',responseOffset:1,unitScale:.001};
assert(parameterDrag(time)(1) < 1.1, '1 ms attack needs sub-0.1 ms precision per pixel');
assert(parameterDrag(time)(10) < 2.1, 'Ten pixels must not turn a short attack into hundreds of ms');
const seconds = {...time,value:.001,max:30,step:.00001,responseOffset:.001,unitScale:1};
assert(Math.abs(parameterDrag(time)(20)/1000-parameterDrag(seconds)(20))<1e-10, 'ms and s must feel identical');
const level={...p,name:'level',value:-6,min:-120,max:24,step:.1,response:'decibels'};
assert(parameterDrag(level)(1)-level.value<=.3, 'Mixing level must not jump a dB per pixel');
assert(parameterMapping(level).toPosition(-60)<.05, 'Near-silent tail must occupy very little travel');
assert(parameterMapping(level).toPosition(6)-parameterMapping(level).toPosition(-36)>.7, 'Most travel belongs to mixing levels');
const hz={...p,value:200,min:20,max:20000,step:.001,scale:'log'};
const ratioA=parameterDrag(hz)(12)/200, ratioB=parameterDrag({...hz,value:2000})(12)/2000;
assert(Math.abs(ratioA-ratioB)<.00001, 'Equal frequency travel must produce equal ratios');
const khz={...hz,value:.2,min:.02,max:20,step:.000001};
assert(Math.abs(parameterDrag(khz)(12)*1000-parameterDrag(hz)(12))<.00001);
for (const param of [time,level,{...p,min:0,max:16,step:.000001,response:'gain'}, {...p,min:1/64,max:64,value:1,response:'log'}]) {
 const map=parameterMapping(param); let previous=param.min;
 for(let i=0;i<=1000;i++) {const v=map.fromPosition(i/1000); assert(v>=previous-1e-10); assert(Math.abs(map.toPosition(v)-i/1000)<1e-8); previous=v;}
}
assert.equal(parameterDrag({...p,name:'unison',discrete:true,min:1,max:8,value:3,step:1})(12),4);
assert.deepEqual(parameterInput('0.00125s',time,'ms'),{value:1.25,literal:'1.25ms'});
assert.deepEqual(parameterInput('2khz',hz,'hz'),{value:2000,literal:'2000hz'});
assert.deepEqual(parameterInput('-120db',level,'db'),{value:-120,literal:'-120db'});
for(const input of ['Infinity','1e999','2 + 3','alert(1)','10hz','30001ms']) assert('error' in parameterInput(input,time,'ms'));
assert('error' in parameterInput('2.5',{...p,discrete:true,max:8},''));
assert.deepEqual(parameterInput('0.123456789123456',p,''),{value:0.123456789123456,literal:'0.123456789123456'});
assert.deepEqual(parameterInput('0.51725',p,''),{value:.51725,literal:'0.51725'}, 'Typed precision must not be quantized to drag step');
const source='x=0.5\ny=200hz\nout=x*y\n';
const params=[p,{...p,id:'freq',name:'Frequency',from:8,to:13,value:200,min:20,max:20000}];
const projected=projectParameters(source,'x=0.1234\ny=2e3hz\nout=x*y\n',params);
assert(projected); assert.equal(projected[0].value,.1234); assert.equal(projected[1].value,2000);
assert.equal('x=0.1234\ny=2e3hz\nout=x*y\n'.slice(projected[1].from,projected[1].to),'2e3hz');
for (const draft of [source.replace('x*y','x+y'),source.replace('200hz','200khz'),source.replace('0.5','NaN'),source.replace('0.5','2'),source+'// comment',source.replace('200hz','200hz + 1')]) assert.equal(projectParameters(source,draft,params),null,'Structural or invalid edits must not reuse spans');
assert.equal(projectParameters(source,source,params),params);
const duplicate=projectParameters(source,source.replace('0.5','.25'),[...params,{...p,id:'shared',name:'Shared'}]);
assert.equal(duplicate[2].id,'shared'); assert.equal(duplicate[2].value,.25);
console.log('Parameter gestures: musical time/level/gain response, unit-invariant travel, precise input, log/linear/signed/relative ranges, Shift, integer counts, endpoint reversal and safe draft span projection passed.');
