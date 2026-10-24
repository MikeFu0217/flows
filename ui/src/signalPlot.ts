import { telemetry, signalData } from './telemetry';
import { beatScopeAvailable } from './observation';
import { BeatEnvelope, curveTangents, clamp, DB_CEILING, DB_FLOOR, follow, followPeak, linearPlotRange, logPosition, spectrumBins } from './plotMath';

type Scene = { captured: boolean; key: string; time: number; sequence: number; levels: number[]; peaks: number[]; target: number[]; signed: boolean; range: number; envelope: BeatEnvelope };
const scenes = new WeakMap<HTMLCanvasElement, Scene>();
export function drawSignalPlot(canvas: HTMLCanvasElement, name: string, color = '#bcefc4', spectrum = false, scope: 'beat' | 'detail' = 'beat') {
  const ctx = canvas.getContext('2d'); if (!ctx) return;
  const frame = telemetry(), signal = signalData(name);
  const intrinsic = signal?.shape === 'spectrum' || signal?.shape === 'mel_spectrum';
  spectrum ||= intrinsic;
  const values = spectrum ? signal?.spectrum ?? (intrinsic ? signal?.values : undefined) : signal?.values;
  const w = canvas.width, h = canvas.height, dpr = window.devicePixelRatio || 1;
  const textScale = (parseFloat(canvas.ownerDocument?.documentElement.style.fontSize) || 12) / 12;
  const large = w / dpr > 360 && h / dpr > 75;
  const pad = large ? 20*dpr : 2*dpr, bottom = large ? h-21*dpr*textScale : h-2*dpr, top = large ? 9*dpr : 2*dpr;
  const autoLinear = !!signal?.signedSpectrum && (!spectrum || intrinsic);
  const left = large ? (autoLinear ? 64 : 35)*dpr*textScale : 0, right = w-(large ? 14*dpr : 0), width = right-left, height = bottom-top;
  ctx.clearRect(0,0,w,h);
  if (!signal || !values?.length || signal?.valid === false || signal?.error || signal?.shape === 'scalar') { scenes.delete(canvas); return; }
  const key = JSON.stringify([name, spectrum, scope, frame.programRevision, frame.generation, frame.parameterEpoch, frame.observationEpoch, frame.captureMode, frame.captureMode === 'key' ? frame.keyEpoch : 0]);
  let scene = scenes.get(canvas);
  const now = performance.now(), frozen = frame.frozen || frame.captureHeld || signal.held || spectrum && signal.spectrumHeld;
  if (!scene || scene.key !== key) { scene = { captured: false, key, time: now, sequence: -1, levels: [], peaks: [], target: [], signed: false, range: 0, envelope: new BeatEnvelope() }; scenes.set(canvas,scene); }
  const acquired = !!frame.captureHeld && !scene.captured && !frame.frozen;
  scene.captured = !!frame.captureHeld;
  // Commit the completed acquisition once, including its last waveform bins.
  if (acquired) scene.envelope = new BeatEnvelope();
  const dt = Math.min(.1, Math.max(0,(now-scene.time)/1000)); scene.time = now;
  ctx.lineJoin = 'round'; ctx.lineCap = 'round'; ctx.font = `${9*dpr*textScale}px ui-monospace, monospace`;
  const line = (x1:number,y1:number,x2:number,y2:number,alpha=.1) => { ctx.strokeStyle=`rgba(170,202,191,${alpha})`;ctx.lineWidth=dpr*.6;ctx.beginPath();ctx.moveTo(x1,y1);ctx.lineTo(x2,y2);ctx.stroke(); };
  const text = (label:string,x:number,y:number) => {ctx.fillStyle='rgba(170,194,182,.62)';ctx.fillText(label,x,y);};
  const curve = (points: number[], x: (i: number) => number, y: (value: number) => number, join = false) => {
    if (!points.length) return;
    const tangents = curveTangents(points);
    if (join) ctx.lineTo(x(0), y(points[0])); else ctx.moveTo(x(0), y(points[0]));
    for (let i = 1; i < points.length; i++) {
      const x0 = x(i-1), x1 = x(i), dx = (x1-x0)/3;
      ctx.bezierCurveTo(x0+dx, y(points[i-1]+tangents[i-1]/3), x1-dx, y(points[i]-tangents[i]/3), x1, y(points[i]));
    }
  };
  const stroke = (points:number[], y:(value:number)=>number, alpha=1, fill=false) => {
    if (!points.length) return;
    ctx.beginPath();
    if (spectrum) curve(points, i => left+i/Math.max(1,points.length-1)*width, y);
    else points.forEach((v,i) => {const x=left+i/Math.max(1,points.length-1)*width; i ? ctx.lineTo(x,y(v)):ctx.moveTo(x,y(v));});
    ctx.globalAlpha=alpha;ctx.strokeStyle=color;ctx.lineWidth=(large?1.25:1)*dpr;ctx.stroke();
    if(fill){ctx.lineTo(right,bottom);ctx.lineTo(left,bottom);ctx.closePath();const gradient=ctx.createLinearGradient(0,top,0,bottom);gradient.addColorStop(0,color+'75');gradient.addColorStop(1,color+'08');ctx.fillStyle=gradient;ctx.fill();}
    ctx.globalAlpha=1;
  };
  if(spectrum) {
    const maxHz = Math.min(24000,(frame.sampleRate || 48000)/2), frequencies = (intrinsic ? signal.frequencies : signal.spectrumFrequencies) || [];
    // A waveform difference may be signed, but its FFT magnitude is not.
    scene.signed ||= intrinsic && !!signal.signedSpectrum || values.some(v=>v<0);
    for(const hz of [20,50,100,200,500,1000,2000,5000,10000,20000]) {
      if(hz>maxHz) continue;
      const x=left+logPosition(hz,maxHz)*width;line(x,top,x,bottom,.075);
      if(large && [50,100,500,1000,5000,10000,20000].includes(hz)) {ctx.textAlign='center';text(hz>=1000?`${hz/1000}k`:`${hz}`,x,h-5*dpr);}
    }
    ctx.textAlign='right';
    if(scene.signed) {
      // Signed analysis differences stay linear and centered. A negative result
      // must never silently become a positive magnitude in a dB analyzer.
      const scale=scene.range=linearPlotRange(values,scene.range);
      line(left,(top+bottom)/2,right,(top+bottom)/2,.3);
      ctx.beginPath();values.forEach((v,i)=>{const x=left+logPosition(frequencies[i]??20,maxHz)*width,y=(top+bottom)/2-clamp(v/scale,-1,1)*height*.46;i?ctx.lineTo(x,y):ctx.moveTo(x,y);});ctx.strokeStyle=color;ctx.lineWidth=dpr;ctx.stroke();
      if(large){const label=Number(scale.toPrecision(3)).toString();text(`+${label}`,left-8*dpr,top+8*dpr);text('0',left-8*dpr,(top+bottom)/2);text(`−${label}`,left-8*dpr,bottom);text('SIGNED · LINEAR · AUTO',right,top+10*dpr);}return;
    }
    const y=(db:number)=>top+(DB_CEILING-db)/(DB_CEILING-DB_FLOOR)*height;
    for(const db of [0,-24,-48,-72]){line(left,y(db),right,y(db),.09);if(large)text(String(db),left-7*dpr,y(db)+3*dpr);}
    if(scene.sequence!==frame.sequence) {
      scene.target=spectrumBins(values,frequencies,maxHz,256,signal.shape==='mel_spectrum');scene.sequence=frame.sequence;
      if(!scene.levels.length){scene.levels=[...scene.target];scene.peaks=[...scene.target];}
    }
    if(acquired){scene.levels=[...scene.target];scene.peaks=scene.target.map((v,i)=>Math.max(v,scene!.peaks[i] ?? v));}
    const silent = scene.target.every(value => value === DB_FLOOR);
    if(!frozen) for(let i=0;i<scene.target.length;i++) {scene.levels[i]=follow(scene.levels[i],scene.target[i],dt);scene.peaks[i]=followPeak(scene.peaks[i],scene.levels[i],dt,silent);}
    stroke(scene.peaks,y,.22);stroke(scene.levels,y,1,true);
    if(large){ctx.textAlign='right';text(signal.shape==='mel_spectrum'?'dB · POWER':'dBFS',right,top+10*dpr);}
  } else {
    const auto = signal.kind === 'analysis' && !!signal.signedSpectrum;
    const amplitude = auto ? scene.range=linearPlotRange([signal.peak??0,...values],scene.range) : signal.kind === 'control' ? Math.max(1,signal.peak??1) : 1;
    const center=(top+bottom)/2, y=(v:number)=>center-clamp(v/amplitude,-1.15,1.15)*height*.43;
    line(left,center,right,center,.22);if(large){line(left,y(amplitude),right,y(amplitude),.08);line(left,y(-amplitude),right,y(-amplitude),.08);ctx.textAlign='right';text(`+${Number(amplitude.toPrecision(3))}`,left-7*dpr,y(amplitude)+3*dpr);text('0',left-7*dpr,center+3*dpr);text(`−${Number(amplitude.toPrecision(3))}`,left-7*dpr,y(-amplitude)+3*dpr);}
    if(large && auto) text('LINEAR · AUTO',right,top+10*dpr);
    if(scope==='beat' && beatScopeAvailable(frame, signal) && signal.beatMin?.length && signal.beatMax?.length && signal.beatAge?.length) {
      for(let i=0;i<=4;i++){const x=left+i*width/4;line(x,top,x,bottom,i%2===0?.15:.08);if(large){ctx.textAlign=i===4?'right':'left';text(['1','e','&','a','2'][i],x+2*dpr,h-5*dpr);}}
      const envelope = scene.envelope;
      envelope.update(signal.beatMin, signal.beatMax, signal.beatAge, dt, !!frozen);
      const { low, high, ages, opacity } = envelope;
      // Continuous upper/lower contours replace separate rectangles. Interpolate
      // only inside contiguous captured regions; gaps remain visibly empty.
      for (let first = 0; first < low.length;) {
        if (ages[first] < 0) { first++; continue; }
        let end = first+1;
        while (end < low.length && ages[end] >= 0) end++;
        const xAt = (i: number) => left+(i+.5)/low.length*width;
        const upper = high.slice(first,end), lower = low.slice(first,end).reverse();
        ctx.beginPath();
        ctx.moveTo(left+first/low.length*width,y(upper[0]));
        curve(upper, i=>xAt(first+i),y,true);
        ctx.lineTo(left+end/low.length*width,y(upper[upper.length-1]));
        ctx.lineTo(left+end/low.length*width,y(lower[0]));
        curve(lower,i=>xAt(end-1-i),y,true);
        ctx.lineTo(left+first/low.length*width,y(lower[lower.length-1]));ctx.closePath();
        const tint=ctx.createLinearGradient(left,0,right,0);
        for(let i=first;i<end;i++) tint.addColorStop((i+.5)/low.length,color+Math.round(clamp(opacity[i],0,1)*185).toString(16).padStart(2,'0'));
        ctx.fillStyle=tint;ctx.fill();
        ctx.save();ctx.clip();
        const depth=ctx.createLinearGradient(0,top,0,bottom);
        depth.addColorStop(0,color+'12');depth.addColorStop(.5,color+'00');depth.addColorStop(1,color+'12');
        ctx.fillStyle=depth;ctx.fillRect(left,top,width,height);ctx.restore();
        ctx.strokeStyle=color+'40';ctx.lineWidth=.8*dpr;ctx.stroke();
        first=end;
      }
      ctx.globalAlpha=1;const x=left+(signal.beatPhase??0)*width;
      const glow=ctx.createLinearGradient(Math.max(left,x-pad),0,x,0);glow.addColorStop(0,color+'00');glow.addColorStop(1,color+'20');ctx.fillStyle=glow;ctx.fillRect(Math.max(left,x-pad),top,Math.min(pad,x-left),height);
      line(x,top,x,bottom,.65);
    } else {
      const wave=signal.detail?.length?signal.detail:values;
      for(let i=1;i<4;i++)line(left+width*i/4,top,left+width*i/4,bottom,.08);
      stroke(wave,y);
      if(large){ctx.textAlign='left';text('0',left,h-5*dpr);ctx.textAlign='right';text(`${((signal.detailSeconds??((signal.sampleCount??0)/(frame.sampleRate??48000)))*1000).toFixed(1)} ms`,right,h-5*dpr);}
    }
  }
}
