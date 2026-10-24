import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import ts from 'typescript';
const output=ts.transpileModule(readFileSync(new URL('../src/plotMath.ts',import.meta.url),'utf8'),{compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.ESNext}}).outputText;
const {logPosition,decibels,follow,followPeak,linearPlotRange,spectrumBins,curveTangents,BeatEnvelope}=await import(`data:text/javascript;base64,${Buffer.from(output).toString('base64')}`);
assert.equal(linearPlotRange([.003864,-.002]),.005,'The 800 Hz filter spectrum difference must be visible on a labelled linear axis');
assert.equal(linearPlotRange([-.192191,.1]),.5,'Signed waveform differences fit without clipping');
assert.equal(linearPlotRange([.001],.005),.005,'A capture keeps its scale as the signal decays');
assert.equal(linearPlotRange([0,1e-15,NaN,Infinity]),1e-5,'Silence and numerical dust do not receive unlimited amplification');
for (const tick of [1e-5,2e-5,5e-5,1e-4,2e-4,5e-4,.001,.002,.005,.01,.1,1,10]) {
  assert.equal(linearPlotRange([tick/1.1]),tick,'Decimal tick boundaries must agree across JavaScript engines');
  assert(linearPlotRange([tick/1.1*(1+1e-8)])>tick,'A genuinely larger signal must advance the display range');
}
assert.equal(logPosition(20,20000),0);assert.equal(logPosition(20000,20000),1);
assert(Math.abs(logPosition(200,20000)-1/3)<1e-10);
assert.equal(decibels(1),0);assert.equal(decibels(0),-90);
assert(Math.abs(decibels(.1)+20)<1e-10);assert(Math.abs(decibels(.1,true)+10)<1e-10);
assert(Math.abs(follow(follow(-60,-12,.01),-12,.02)-follow(-60,-12,.03))<1e-10);
assert(follow(-60,-12,.03)>-48);assert(follow(-12,-60,.03)>-24);
assert.equal(followPeak(0,-90,.5),-7.5,'Sound keeps the normal slow peak trail');
assert.equal(followPeak(0,0,.5,true),0,'A peak cannot fall below the displayed signal');
assert.equal(followPeak(0,-90,1,true),-90,'Measured silence drains the old peak before idle');
assert.equal(followPeak(followPeak(0,-90,.2,true),-90,.3,true),followPeak(0,-90,.5,true),'Silent peak decay is frame-rate independent');
assert.equal(followPeak(-12,-24,-1,true),-12,'A negative elapsed time cannot increase a peak');
let releasedLevel=0,releasedPeak=0;
for(let tick=0;tick<90;tick++) {
  releasedLevel=follow(releasedLevel,-90,1/60);
  releasedPeak=followPeak(releasedPeak,releasedLevel,1/60,true);
  assert(releasedPeak>=releasedLevel,'Peak trail stays above the smoothed spectrum');
}
assert(releasedLevel<-88 && releasedPeak<-88,'The live spectrum and peak trail settle together during the 1.5-second quiet drain');
const bins=spectrumBins([0,1,.01,.1],[0,100,1000,10000],20000,256);
assert.equal(Math.max(...bins),0);assert(bins.every(Number.isFinite));
assert.deepEqual(spectrumBins([NaN,Infinity],[50,100],20000,4),[-90,-90,-90,-90]);
console.log('Plot math: log axis, fixed amplitude/power dB, frame-rate-independent ballistics and peak-preserving bins passed.');

for(const values of [[0,1,0,.5],[0,.1,.9,1],[1,1,1],[0,-1,0],[-1,.5,.5,-.8]]) {
  const slopes=curveTangents(values);
  for(let i=1;i<values.length;i++) for(let t=0;t<=1;t+=.01) {
    const a=values[i-1],b=values[i],u=1-t;
    const v=u*u*u*a+3*u*u*t*(a+slopes[i-1]/3)+3*u*t*t*(b-slopes[i]/3)+t*t*t*b;
    assert(v>=Math.min(a,b)-1e-9 && v<=Math.max(a,b)+1e-9,'Rounded contour overshot measured knots');
  }
}
const envelope=new BeatEnvelope();
envelope.update([-.2,-.3],[.2,.3],[0,1],0);
envelope.update([-.8,-.9],[.8,.9],[0,0],.016);
assert(envelope.high[0]>.2 && envelope.high[0]<.8);
assert(envelope.opacity[1]>.22 && envelope.opacity[1]<.85);
assert(envelope.low.every((v,i)=>v<=envelope.high[i]));
const frozen=JSON.stringify(envelope);
envelope.update([-1,-1],[1,1],[1,0],.1,true);assert.equal(JSON.stringify(envelope),frozen);
envelope.update([0,0],[0,0],[-1,0],.016);assert.equal(envelope.opacity[0],0);assert.equal(envelope.ages[0],-1);
const a=new BeatEnvelope(),b=new BeatEnvelope();
a.update([-.2],[.2],[0],0);b.update([-.2],[.2],[0],0);
a.update([-.8],[.8],[1],.01);a.update([-.8],[.8],[1],.02);b.update([-.8],[.8],[1],.03);
assert(Math.abs(a.high[0]-b.high[0])<1e-10);assert(Math.abs(a.opacity[0]-b.opacity[0])<1e-10);
console.log('Smooth contours: no overshoot, envelope attack/release, age crossfade, missing capture, exact Hold and frame-rate invariance passed.');

// Exercise the actual renderer: an 800 Hz difference used to occupy less than
// one pixel, and the unsigned FFT of a signed waveform inherited a linear axis.
const moduleUrl = source => `data:text/javascript;base64,${Buffer.from(source).toString('base64')}`;
const plotSource = ts.transpileModule(readFileSync(new URL('../src/signalPlot.ts',import.meta.url),'utf8'),{compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.ESNext}}).outputText
  .replace("'./telemetry'",JSON.stringify(moduleUrl('export const telemetry=()=>globalThis.plotFrame; export const signalData=name=>globalThis.plotFrame.signals.find(s=>s.name===name);')))
  .replace("'./observation'",JSON.stringify(moduleUrl('export const beatScopeAvailable=()=>false;')))
  .replace("'./plotMath'",JSON.stringify(moduleUrl(output)));
const {drawSignalPlot}=await import(moduleUrl(plotSource));
globalThis.window={devicePixelRatio:1};
const labels=[],paths=[];
const context={clearRect(){labels.length=0;paths.length=0;},beginPath(){paths.push([]);},moveTo(x,y){paths.at(-1).push([x,y]);},lineTo(x,y){paths.at(-1).push([x,y]);},bezierCurveTo(){},stroke(){},fill(){},closePath(){},fillText(label){labels.push(label);},createLinearGradient(){return {addColorStop(){}};}};
const canvas={width:800,height:240,getContext:()=>context};
const measured=[.003864,-.002,0];
globalThis.plotFrame={sequence:1,sampleRate:48000,keyEpoch:1,captureMode:'key',signals:[{name:'spectrum_diff',kind:'analysis',shape:'spectrum',signedSpectrum:true,values:measured,frequencies:[100,1000,10000]}]};
drawSignalPlot(canvas,'spectrum_diff');
assert(labels.includes('+0.005') && labels.includes('−0.005') && labels.includes('SIGNED · LINEAR · AUTO'));
const trace=paths.at(-1);
assert(Math.abs(trace[0][1]-trace[2][1])>60,'A real small difference must be visibly larger than one pixel');
assert(trace[0][1]<trace[2][1] && trace[1][1]>trace[2][1],'Positive and negative differences stay on opposite sides of zero');
assert.deepEqual(measured,[.003864,-.002,0],'Display scaling must not modify measured values');
globalThis.plotFrame={...globalThis.plotFrame,parameterEpoch:1,signals:[{name:'spectrum_diff',kind:'analysis',shape:'spectrum',signedSpectrum:true,values:[0,0],frequencies:[100,1000]}]};
drawSignalPlot(canvas,'spectrum_diff');
assert.equal(paths.at(-1)[0][1],paths.at(-1)[1][1],'A true zero remains a flat line');
globalThis.plotFrame={...globalThis.plotFrame,signals:[{name:'diff',kind:'analysis',shape:'waveform',signedSpectrum:true,values:[-.02,.02],peak:.02,spectrum:[.01,.001],spectrumFrequencies:[100,1000]}]};
drawSignalPlot(canvas,'diff',undefined,true);
assert(labels.includes('dBFS') && !labels.includes('SIGNED · LINEAR · AUTO'),'FFT magnitude of diff uses the ordinary dB spectrum');
drawSignalPlot(canvas,'diff',undefined,false);
assert(labels.includes('LINEAR · AUTO') && labels.includes('+0.05'),'Waveform differences also have a readable labelled scale');
globalThis.plotFrame.signals[0]={name:'raw',kind:'audio',shape:'waveform',values:[-.02,.02],peak:.02};
drawSignalPlot(canvas,'raw');
assert(labels.includes('+1') && !labels.includes('LINEAR · AUTO'),'Ordinary audio keeps its calibrated full-scale waveform axis');
canvas.ownerDocument={documentElement:{style:{fontSize:'24px'}}};
drawSignalPlot(canvas,'raw');
assert.equal(context.font,'18px ui-monospace, monospace','Plot labels follow 200% interface text size');
assert(paths.flat().every(point=>point.every(Number.isFinite)),'Scaled plot coordinates remain finite');
canvas.ownerDocument.documentElement.style.fontSize='7.5px';
drawSignalPlot(canvas,'raw');
assert.equal(context.font,'5.625px ui-monospace, monospace','Plot labels follow the minimum interface text size');
delete globalThis.plotFrame;
delete globalThis.window;
console.log('Difference renderer: visible signed 800 Hz result, labelled automatic range, exact zero, unchanged samples and unsigned waveform FFT passed.');
