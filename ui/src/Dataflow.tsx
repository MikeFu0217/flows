import { openParameterInput } from './ParameterInput';
import { captureState, heldLabel } from './observation';
import { CSSProperties, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Choice, FlowGraph, FlowNode, Parameter, Signal, Projection } from './types';
import { layoutFlow, pathToNode, visibleFlowGraph } from './flowLayout';
import { drawWave, onPlot, onTelemetry, signalData, telemetry } from './telemetry';
import { Icon } from './Icon';
import './Dataflow.css';
import { ObservationHold, ObservationVoice, ObservationProjection } from './Signals';
import { displayKey, signalDisplay, onSignalDisplay, openSignalDisplayMenu } from './signalDisplay';
import { parameterDrag, parameterMapping, normaliseParameter, parameterStep, projectParameters } from './parameterDrag';
import { parameterDefaultLiteral } from './parameterDefault';
import { ChoiceSelect } from './ChoiceMenu';
import { applyChoice } from './choice';

export interface DataflowProps {
  fontScale?: number;
  graph?: FlowGraph | null;
  parameters: Parameter[];
  choices: Choice[];
  source: string;
  documentId: string;
  editable: boolean;
  status: string;
  audition?: string | null;
  listenPending?: boolean;
  signals?: Signal[];
  observation?: { projection: Projection; onProjectionChange: (projection: Projection) => void; voice: number; limit: number; roots: number; skipped: Record<string, string>; error?: string; notice?: string; onVoiceChange: (voice: number) => void };
  onChange: (source: string, gestureId?: string, phase?: string) => void;
  onInspect: (name: string) => void;
  onReveal: (from: number, to: number) => void;
  onListen: (name: string, enabled: boolean) => void;
  onObserve: (names: string[]) => void;
}
const EMPTY_GRAPH: FlowGraph = { nodes: [], edges: [], output: '' };
const pretty = (value: number) => Number.isFinite(value) ? Number(value.toPrecision(8)).toString() : '—';
const suffixOf = (source: string, parameter: Parameter) => source.slice(parameter.from, parameter.to).match(/[a-zA-Z]+$/)?.[0] || '';
const opLabel: Record<string, string> = {
  unison_sine: 'UNISON SINE', unison_saw: 'UNISON SAW', lfo: 'LFO', distortion: 'DISTORTION', stereo: 'STEREO', left: 'LEFT', right: 'RIGHT', mid: 'MID', side: 'SIDE',
  sine: 'SINE OSCILLATOR', saw: 'SAW OSCILLATOR', adsr: 'ENVELOPE',
  filter_lowpass: 'LOWPASS', filter_highpass: 'HIGHPASS',
  add: 'SUM', subtract: 'DIFFERENCE', multiply: 'MULTIPLY', divide: 'DIVIDE',
  key_pitch: 'PITCH', key_gate: 'GATE', key_trigger: 'TRIGGER', key_velocity: 'VELOCITY',
  parameter: 'CONSTANT', alias: 'SIGNAL', output: 'INSTRUMENT OUT',
  power: 'POWER', spectrum: 'SPECTRUM', mel_spectrum: 'MEL SPECTRUM', probe: 'PROBE',
};
const colour = (kind: string) => kind === 'audio' ? '#91e1be' : kind === 'analysis' ? '#b9a0ef' : '#eac38b';

// Values stay in their authored unit (ms, khz, dB...), using compiler-provided
// source spans and ranges. A complete pointer gesture is one native undo entry.
function ParameterKnob({ parameter, source, documentId, disabled, onChange, large = false }: {
  parameter: Parameter; source: string; documentId: string; disabled: boolean;
  onChange: DataflowProps['onChange']; large?: boolean;
}) {
  const latest = useRef({ parameter, source, documentId, disabled, onChange });
  latest.current = { parameter, source, documentId, disabled, onChange };
  const [preview, setPreview] = useState<number | null>(null), [dragging, setDragging] = useState(false);
  const inputCleanup = useRef<() => void>();
  const cleanup = useRef<() => void>(), lastTap = useRef(0);
  const gesture = useRef<{ original: string; last: string; id: string; documentId: string; parameter: Parameter; suffix: string }>();
  useEffect(() => () => { cleanup.current?.(); inputCleanup.current?.(); }, []);
  useEffect(() => { inputCleanup.current?.(); }, [documentId, parameter.id]);
  const suffix = gesture.current?.suffix ?? suffixOf(source, parameter);
  const value = dragging ? preview ?? parameter.value : parameter.value;
  const fixed = parameter.hasRange && parameter.min === parameter.max;
  const ranged = parameter.hasRange && Number.isFinite(parameter.min) && Number.isFinite(parameter.max) && parameter.max > parameter.min;
  const fraction = ranged ? parameterMapping(parameter).toPosition(value) : .5 + Math.atan(value / Math.max(1, Math.abs(parameter.value))) / Math.PI;
  const angle = -135 + Math.max(0, Math.min(1, fraction)) * 270;
  const replacement = (original: string, p: Parameter, next: number | string, unit: string) => original.slice(0, p.from) + next + unit + original.slice(p.to);
  const editValue = (anchor: HTMLElement) => {
    if (disabled || fixed) return;
    inputCleanup.current?.();
    const original = source, originalId = documentId;
    inputCleanup.current = openParameterInput(anchor, parameter, suffix, literal => {
      if (latest.current.disabled || latest.current.source !== original || latest.current.documentId !== originalId) return false;
      if (literal !== original.slice(parameter.from, parameter.to)) latest.current.onChange(replacement(original, parameter, literal, ''), `flow-input-${Date.now()}-${parameter.id}`, 'commit');
      return true;
    });
  };
  const keyboardChange = (key: string, fine: boolean) => {
    if (disabled || fixed) return;
    const p = parameter;
    const step = parameterStep(p, fine);
    const direction = key === 'ArrowUp' || key === 'ArrowRight' ? 1 : -1;
    const next = key === 'Home' && ranged ? p.min : key === 'End' && ranged ? p.max : normaliseParameter(p.value + direction * step, p, fine);
    if (next !== p.value) onChange(replacement(source, p, next, suffix), `flow-key-${Date.now()}-${p.id}`, 'commit');
  };
  return <div className={`flow-parameter ${large ? 'large' : ''} ${dragging ? 'adjusting' : ''}`}>
    <button type="button" className="flow-knob" role="slider" aria-label={`Adjust ${parameter.name || parameter.id}`}
      data-parameter-id={parameter.id} aria-valuenow={value} aria-valuetext={`${pretty(value)}${suffix}`}
      aria-valuemin={ranged ? parameter.min : undefined} aria-valuemax={ranged ? parameter.max : undefined}
      aria-orientation="vertical" disabled={fixed || disabled && !dragging}
      title={`${parameter.name || parameter.id} · ${ranged ? `${parameter.min}–${parameter.max}${suffix}` : 'relative'} · drag horizontally or vertically · ⇧ fine · click value to type${parameter.hasDefault ? ' · double-click for default' : ''}`}
      onKeyDown={event => {
        if (event.key === 'Enter') { event.preventDefault(); event.stopPropagation(); editValue(event.currentTarget); return; }
        if (['ArrowUp', 'ArrowRight', 'ArrowDown', 'ArrowLeft', 'Home', 'End'].includes(event.key)) {
          event.preventDefault(); event.stopPropagation(); keyboardChange(event.key, event.shiftKey);
        }
      }}
      onPointerDown={event => {
        if (event.button !== 0 || disabled || fixed || parameter.from < 0 || parameter.to > source.length) return;
        // Cancelling pointerdown suppresses the browser's own dblclick, so a
        // second press inside the double-click window is detected here.
        const doubleTap = Date.now() - lastTap.current < 400;
        lastTap.current = 0;
        if (doubleTap) {
          const reset = parameterDefaultLiteral(parameter);
          if (reset !== null && `${reset}${suffix}` !== source.slice(parameter.from, parameter.to)) {
            event.preventDefault(); event.stopPropagation();
            onChange(replacement(source, parameter, reset, suffix), `flow-default-${Date.now()}-${parameter.id}`, 'commit');
            return;
          }
        }
        event.preventDefault(); event.stopPropagation(); cleanup.current?.();
        event.currentTarget.focus();
        const start = { original: source, last: source, id: `flow-drag-${Date.now()}-${parameter.id}`, documentId, parameter: { ...parameter }, suffix };
        gesture.current = start; setDragging(true); setPreview(parameter.value);
        let axis: 'x' | 'y' | undefined;
        let moved = false, previousX = event.clientX, previousY = event.clientY, currentValue = parameter.value, finished = false;
        const adjust = parameterDrag(parameter), pointerId = event.pointerId;
        const ownSources = new Set([source]);
        const ownsSource = () => latest.current.documentId === start.documentId && ownSources.has(latest.current.source);
        const detach = () => {
          window.removeEventListener('pointermove', move); window.removeEventListener('pointerup', up);
          window.removeEventListener('pointercancel', abort); window.removeEventListener('keydown', escape, true); window.removeEventListener('blur', blur);
          document.body.classList.remove('flow-knob-dragging'); cleanup.current = undefined;
        };
        const finish = (cancel = false) => {
          if (finished) return; finished = true; detach();
          if (ownsSource())
            latest.current.onChange(cancel ? start.original : start.last, start.id, cancel ? 'cancel' : 'commit');
          if (!cancel && !moved) lastTap.current = Date.now();
          gesture.current = undefined; setDragging(false); setPreview(null);
        };
        const move = (next: PointerEvent) => {
          if (next.pointerId !== pointerId) return;
          if (!ownsSource()) { finish(); return; }
          const dx = next.clientX - previousX, dy = previousY - next.clientY; previousX = next.clientX; previousY = next.clientY;
          if (!axis && (dx || dy)) axis = Math.abs(dx) > Math.abs(dy) ? 'x' : 'y';
          const delta = axis === 'x' ? dx : dy;
          if (!delta) return;
          moved = true;
          const p = start.parameter, value = adjust(delta, next.shiftKey);
          if (!Number.isFinite(value) || currentValue === value) return;
          currentValue = value; setPreview(value);
          start.last = replacement(start.original, p, value, start.suffix);
          ownSources.add(start.last); latest.current.onChange(start.last, start.id);
        };
        const up = (event: PointerEvent) => { if (event.pointerId === pointerId) finish(); }, abort = (event: PointerEvent) => { if (event.pointerId === pointerId) finish(true); };
        const blur = () => finish();
        const escape = (key: KeyboardEvent) => { if (key.key === 'Escape') { key.preventDefault(); key.stopPropagation(); finish(true); } };
        // Unmount must never write an old document back into the new editor.
        cleanup.current = detach; document.body.classList.add('flow-knob-dragging');
        window.addEventListener('pointermove', move); window.addEventListener('pointerup', up); window.addEventListener('pointercancel', abort); window.addEventListener('keydown', escape, true); window.addEventListener('blur', blur);
      }}>
      <svg viewBox="0 0 48 48" aria-hidden="true"><circle className="knob-track" cx="24" cy="24" r="19" pathLength="100"/><circle className="knob-progress" cx="24" cy="24" r="19" pathLength="100" style={{ strokeDasharray: `${Math.max(0, Math.min(1, fraction)) * 75} 100` }}/><circle className="knob-face" cx="24" cy="24" r="14"/><path className="knob-pointer" d="M24 14v6" style={{ transform: `rotate(${angle}deg)`, transformOrigin: '24px 24px' }}/></svg>
    </button>
    <span className="flow-parameter-label" title={parameter.name || parameter.id}>{parameter.name || parameter.id.split('.').slice(-2, -1)[0] || 'Value'}</span>
    <button type="button" className="flow-value-input" aria-label={`Set ${parameter.name || parameter.id} value`} title="Type an exact value · Enter to apply" disabled={disabled || fixed} onClick={event => { event.stopPropagation(); editValue(event.currentTarget); }}>{pretty(value)}<small>{suffix}</small></button>
    {dragging && <div className="flow-value-tip"><strong>{pretty(value)}{suffix}</strong><span>{ranged ? `${parameter.min} — ${parameter.max}${suffix}` : 'Relative adjustment'} · ⇧ fine · Esc cancel</span></div>}
  </div>;
}

function MiniSignal({ node, signal, documentId, error, waiting, onSelect, onInspect }: { node: FlowNode; signal?: Signal; documentId: string; error?: string; waiting?: boolean; onSelect: () => void; onInspect: () => void }) {
  const canvas = useRef<HTMLCanvasElement>(null), label = useRef<HTMLSpanElement>(null), preview = useRef<HTMLButtonElement>(null);
  const closeMenu = useRef<() => void>();
  const descriptor = signal ?? { kind: node.kind };
  const key = displayKey(documentId, node.signal);
  useEffect(() => () => closeMenu.current?.(), [key]);
  useEffect(() => {
    const element = canvas.current; if (!element) return;
    const update = () => {
      const mode = signalDisplay(key, descriptor); if (preview.current) preview.current.dataset.viewMode = mode;
      const data = signalData(node.signal), problem = error || telemetry().error || (!waiting ? data?.error : undefined), valid = !waiting && data && data.valid !== false && !problem && (data.shape === 'scalar' || (mode === 'spectrum' ? (data.spectrum ?? (data.shape === 'spectrum' || data.shape === 'mel_spectrum' ? data.values : undefined))?.length : data.values?.length));
      if (problem || waiting) element.getContext('2d')?.clearRect(0, 0, element.width, element.height);
      else drawWave(element, node.signal, colour(node.kind), mode === 'spectrum');
      if (label.current) {
        const frame = telemetry(), state = captureState(frame, data, mode === 'spectrum');
        const held = state === 'held';
        if(preview.current) preview.current.dataset.captureState = state;
        label.current.textContent = problem || (valid ? data.shape === 'scalar' ? pretty(data.scalar ?? data.value ?? 0) : held ? heldLabel(frame) : '' : state === 'idle' ? 'Play a key or sequencer' : waiting ? 'Select to observe' : mode === 'spectrum' ? data?.captureKind === 'last_voice_control' ? 'Select a Voice' : 'Waiting for spectrum' : 'Waiting for signal');
        label.current.title = problem || (held ? frame.frozen ? 'Display is held. Choose Resume to follow the live signal.' : 'Stored capture while waiting for new measured samples.' : waiting ? 'Select this signal to observe it.' : '');
        label.current.classList.toggle('capture-error', !!problem);
        label.current.dataset.captureState = state;
        label.current.classList.toggle('scalar', !!valid && data?.shape === 'scalar');
      }
    };
    const resize = () => { const box = element.getBoundingClientRect(); element.width = Math.max(1, Math.round(box.width * devicePixelRatio)); element.height = Math.max(1, Math.round(box.height * devicePixelRatio)); update(); };
    const observer = new ResizeObserver(resize); observer.observe(element); const remove = onPlot(update), offDisplay = onSignalDisplay(update); resize();
    return () => { observer.disconnect(); remove(); offDisplay(); };
  }, [node.signal, node.kind, signal, key, error, waiting]);
  return <button ref={preview} type="button" className="flow-scope" aria-label={`Display ${node.signal}`} title="Choose Spectrum or Waveform" onClick={event => { event.stopPropagation(); onSelect(); closeMenu.current = openSignalDisplayMenu(event.currentTarget, key, descriptor, onInspect); }}><canvas ref={canvas}/><span ref={label}/></button>;
}

function ModuleGlyph({ op }: { op: string }) {
  const path = op === 'sine' || op === 'unison_sine' || op === 'lfo' ? 'M1 13C5 0 8 0 12 13s7 13 11 0' : op === 'saw' || op === 'unison_saw' ? 'M1 20 10 4v16L20 4v16' : op === 'adsr' ? 'm1 21 5-18 5 9h6l6 9' : op.includes('filter') ? 'M1 6h8q4 0 6 6l7 9' : op === 'multiply' ? 'm6 6 12 12M6 18 18 6' : 'M3 12h18M12 3v18';
  return <svg className="module-glyph" viewBox="0 0 24 26" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true"><path d={path}/></svg>;
}

export function Dataflow(props: DataflowProps) {
  const latest = useRef(props); latest.current = props;
  const choiceContext = useRef({ documentId: props.documentId, source: props.source, revision: 0, committed: false });
  if (choiceContext.current.documentId !== props.documentId || choiceContext.current.source !== props.source)
    choiceContext.current = { documentId: props.documentId, source: props.source, revision: choiceContext.current.revision + 1, committed: false };
  const cache = useRef<{ documentId: string; graph: FlowGraph; parameters: Parameter[]; choices: Choice[]; source: string }>({ documentId: '', graph: EMPTY_GRAPH, parameters: [], choices: [], source: '' });
  if (cache.current.documentId !== props.documentId) cache.current = { documentId: props.documentId, graph: EMPTY_GRAPH, parameters: [], choices: [], source: props.source };
  if (props.graph?.nodes.length) cache.current = { documentId: props.documentId, graph: props.graph, parameters: props.parameters, choices: props.choices, source: props.source };
  const graph = visibleFlowGraph(cache.current.graph);
  const projected = useMemo(() => projectParameters(cache.current.source, props.source, cache.current.parameters), [cache.current.source, props.source, cache.current.parameters]);
  const parameters = projected ?? cache.current.parameters;
  const parameterMap = useMemo(() => new Map(parameters.map(parameter => [parameter.id, parameter])), [parameters]);
  const choiceMap = useMemo(() => new Map(cache.current.choices.map(choice => [choice.id, choice])), [cache.current.choices]);
  const signature = JSON.stringify({ nodes: graph.nodes.map(node => [node.id, node.op, node.parameterIds.filter(id => parameterMap.has(id)), (node.choiceIds || []).filter(id => choiceMap.has(id))]), edges: graph.edges.map(edge => [edge.id, edge.source, edge.target]) });
  // Node geometry depends on topology, not a source number or a telemetry frame.
  const geometry = useMemo(() => layoutFlow(graph, parameterMap, choiceMap, props.fontScale), [signature, props.fontScale]);
  // Source spans move when a literal changes width; geometry can stay put.
  const currentNodes = new Map(graph.nodes.map(node => [node.id, node]));
  const layout = { ...geometry, nodes: geometry.nodes.map(node => ({ ...node, ...currentNodes.get(node.id) })) };
  const [selected, setSelected] = useState<string | null>(null), [fullscreen, setFullscreen] = useState(false);
  const [camera, setCamera] = useState({ x: 24, y: 24, zoom: 1 }), [viewportSize, setViewportSize] = useState({ width: 1, height: 1 });
  const viewport = useRef<HTMLDivElement>(null), autoFit = useRef(true), panCleanup = useRef<() => void>();
  const framing = useRef<'readable' | 'overview'>('readable');
  const cameraRef = useRef(camera); cameraRef.current = camera;
  const active = useMemo(() => pathToNode(graph, selected), [signature, selected]);
  const selectedNode = graph.nodes.find(node => node.id === selected);
  const allSelectedParameters = selectedNode?.parameterIds.map(id => parameterMap.get(id)).filter((value): value is Parameter => !!value) || [];
  const allSelectedChoices = (selectedNode?.choiceIds || []).map(id => choiceMap.get(id)).filter((value): value is Choice => !!value);
  const fit = useCallback(() => {
    const box = viewport.current?.getBoundingClientRect(); if (!box || box.width < 10 || box.height < 10) return;
    const fitZoom = Math.min((box.width - 32) / layout.width, (box.height - 20) / layout.height);
    const zoom = Math.min(1, Math.max(framing.current === 'overview' ? .08 : .85, fitZoom));
    setCamera({ x: Math.max(16, (box.width - layout.width * zoom) / 2), y: Math.max(12, (box.height - layout.height * zoom) / 2), zoom });
    autoFit.current = true;
  }, [layout.width, layout.height]);
  useEffect(() => {
    const element = viewport.current; if (!element) return;
    const observer = new ResizeObserver(() => { setViewportSize({ width: element.clientWidth, height: element.clientHeight }); if (autoFit.current) fit(); });
    observer.observe(element); fit(); return () => observer.disconnect();
  }, [fit]);
  useEffect(() => {
    setSelected(null); framing.current = 'readable'; autoFit.current = true; fit();
    latest.current.onObserve([]);
  }, [signature, props.documentId]);
  useEffect(() => () => panCleanup.current?.(), []);
  useEffect(() => { const escape = (event: KeyboardEvent) => { if (event.key === 'Escape') setFullscreen(false); }; window.addEventListener('keydown', escape); return () => window.removeEventListener('keydown', escape); }, []);
  const zoomAt = useCallback((zoom: number, x: number, y: number) => {
    setCamera(previous => { const next = Math.max(.08, Math.min(1.8, zoom)); return { zoom: next, x: x - (x - previous.x) * next / previous.zoom, y: y - (y - previous.y) * next / previous.zoom }; });
    autoFit.current = false;
  }, []);
  useEffect(() => {
    const element = viewport.current; if (!element) return;
    const wheel = (event: WheelEvent) => {
      event.preventDefault();
      if (event.ctrlKey || event.metaKey) {
        const rect = element.getBoundingClientRect(); zoomAt(cameraRef.current.zoom * Math.exp(-event.deltaY * .006), event.clientX - rect.left, event.clientY - rect.top);
      } else { autoFit.current = false; setCamera(previous => ({ ...previous, x: previous.x - event.deltaX, y: previous.y - event.deltaY })); }
    };
    element.addEventListener('wheel', wheel, { passive: false }); return () => element.removeEventListener('wheel', wheel);
  }, [zoomAt]);
  const select = (node: FlowNode, focus = false) => {
    setSelected(node.id); props.onObserve([node.signal]);
    if (props.editable && props.graph) props.onReveal(node.from, node.to);
    if (focus || camera.zoom < .65) {
      const placed = layout.nodes.find(value => value.id === node.id);
      if (placed) { autoFit.current = false; setCamera({ zoom: 1, x: viewportSize.width / 2 - placed.x - placed.width / 2, y: viewportSize.height / 2 - placed.y - placed.height / 2 }); }
    }
  };
  const positioned = new Map(layout.nodes.map(node => [node.id, node]));
  const edgePath = (source: string, target: string) => {
    const a = positioned.get(source)!, b = positioned.get(target)!;
    const x1 = a.x + a.width, y1 = a.y + Math.min(a.height / 2, 68 * (props.fontScale ?? 1)), x2 = b.x, y2 = b.y + Math.min(b.height / 2, 68 * (props.fontScale ?? 1));
    const bend = Math.max(42, (x2 - x1) * .5);
    return `M${x1},${y1} C${x1 + bend},${y1} ${x2 - bend},${y2} ${x2},${y2}`;
  };
  const readable = props.editable && projected !== null && props.status !== 'error';
  const choicesReadable = props.editable && !!props.graph && props.status === 'ready';
  const clearSelection = () => { setSelected(null); props.onObserve([]); };
  const selectChoice = (choice: Choice, value: string) => {
    const current = latest.current;
    if (!current.editable || !current.graph || current.status !== 'ready' ||
        current.documentId !== props.documentId || current.source !== props.source || choiceContext.current.committed) return;
    const descriptor = current.choices.find(candidate => candidate.id === choice.id);
    if (!descriptor) return;
    const next = applyChoice(current.source, descriptor, value);
    if (next === null) return;
    choiceContext.current.committed = true;
    current.onChange(next, `flow-choice-${Date.now()}-${choiceContext.current.revision}-${choice.id}`, 'commit');
  };
  const choiceControls = (choices: Choice[], detail = false) => <div className={`flow-choices ${detail ? 'detail' : ''}`}
    onPointerDown={event => event.stopPropagation()} onClick={event => event.stopPropagation()} onDoubleClick={event => event.stopPropagation()}>
    {choices.map(choice => <ChoiceSelect key={`${props.documentId}:${choice.id}`} choice={choice} disabled={!choicesReadable}
      contextKey={choiceContext.current.revision} className="flow-choice-select" onSelect={value => selectChoice(choice, value)}/>)}</div>;
  return <section className={`dataflow ${fullscreen ? 'expanded' : ''}`} aria-label="Signal dataflow"
    onDragOver={event => { if (event.dataTransfer.types.includes('application/flows-signal')) { event.preventDefault(); event.dataTransfer.dropEffect = 'copy'; } }}
    onDrop={event => { const name = event.dataTransfer.getData('application/flows-signal'); const node = graph.nodes.find(value => value.signal === name); if (node) { event.preventDefault(); select(node, true); } }}>
    <header className="dataflow-toolbar"><div className="flow-title"><svg viewBox="0 0 24 24" fill="none" aria-hidden="true"><path d="M3 5h5v5H3zM16 3h5v5h-5zM16 16h5v5h-5zM8 7.5h4l4-2M8 7.5h4v11h4" stroke="currentColor" strokeWidth="1.5" strokeLinejoin="round"/></svg><strong>SIGNAL FLOW</strong></div><div className="flow-legend"><span className="audio">Audio</span><span className="control">Control</span><span className="analysis">Analysis</span></div><div className="spacer"/>{props.observation && <ObservationVoice value={props.observation.voice} limit={props.observation.limit} onChange={props.observation.onVoiceChange}/>}{props.observation && <ObservationProjection value={props.observation.projection} onChange={props.observation.onProjectionChange}/>}<ObservationHold/><div className="flow-camera-controls"><button title="Zoom out signal flow" aria-label="Zoom out signal flow" onClick={() => zoomAt(camera.zoom / 1.2, viewportSize.width / 2, viewportSize.height / 2)}>−</button><button className="flow-zoom-value" title="Actual size" onClick={() => zoomAt(1, viewportSize.width / 2, viewportSize.height / 2)}>{Math.round(camera.zoom * 100)}%</button><button title="Zoom in signal flow" aria-label="Zoom in signal flow" onClick={() => zoomAt(camera.zoom * 1.2, viewportSize.width / 2, viewportSize.height / 2)}>+</button><button className="flow-fit" onClick={() => { framing.current = 'overview'; fit(); }} title="Fit the whole signal flow">Fit</button><button title={fullscreen ? 'Restore split view' : 'Expand signal flow'} onClick={() => { framing.current = fullscreen ? 'readable' : 'overview'; autoFit.current = true; setFullscreen(!fullscreen); }}><Icon name={fullscreen ? 'rows' : 'expand'} size={15}/></button></div></header>
    <div className="dataflow-viewport" ref={viewport} tabIndex={0} aria-label="Signal flow canvas. Drag empty space to pan. Pinch to zoom."
      onKeyDown={event => { if (event.target !== event.currentTarget) return; if (event.key.toLowerCase() === 'f') { event.preventDefault(); framing.current = 'overview'; fit(); } }}
      onPointerDown={event => {
        if (event.button !== 0 || (event.target as Element).closest('.flow-node,.flow-minimap')) return;
        event.preventDefault(); panCleanup.current?.(); const origin = cameraRef.current;
        const startX = event.clientX, startY = event.clientY; let moved = false;
        const base = cameraRef.current; autoFit.current = false; event.currentTarget.classList.add('panning');
        const move = (next: PointerEvent) => { if (Math.hypot(next.clientX - startX, next.clientY - startY) > 3) moved = true; if (moved) setCamera({ zoom: origin.zoom, x: base.x + next.clientX - startX, y: base.y + next.clientY - startY }); };
        const finish = (event?: PointerEvent) => { if (event?.type === 'pointerup' && !moved) clearSelection(); window.removeEventListener('pointermove', move); window.removeEventListener('pointerup', finish); window.removeEventListener('pointercancel', finish); viewport.current?.classList.remove('panning'); panCleanup.current = undefined; };
        panCleanup.current = finish; window.addEventListener('pointermove', move); window.addEventListener('pointerup', finish); window.addEventListener('pointercancel', finish);
      }}>
      {!graph.nodes.length ? <div className="flow-empty"><Icon name="code" size={28}/><strong>{graph.error || (props.status === 'error' ? 'The signal flow will return when the code compiles.' : 'Your code becomes a signal flow.')}</strong><span>Write an Instrument above. Its signals and connections appear here.</span></div> : <>
        <div className={`flow-scene ${selected ? 'has-selection' : ''}`} style={{ width: layout.width, height: layout.height, transform: `translate(${camera.x}px, ${camera.y}px) scale(${camera.zoom})` }}>
          <svg className="flow-wires" width={layout.width} height={layout.height} aria-hidden="true"><defs>{['audio','control','analysis'].map(kind => <marker key={kind} id={`flow-arrow-${kind}`} markerWidth="6" markerHeight="6" refX="5" refY="3" orient="auto"><path d="M0 0 6 3 0 6z" fill={colour(kind)}/></marker>)}</defs>{layout.edges.map(edge => <g key={edge.id} className={`flow-connection ${selected && active.has(edge.source) && active.has(edge.target) ? 'highlighted' : ''} ${selected && (!active.has(edge.source) || !active.has(edge.target)) ? 'dimmed' : ''}`}><path className={`flow-edge ${edge.kind}`} data-from={edge.source} data-to={edge.target} d={edgePath(edge.source, edge.target)} stroke={colour(edge.kind)} markerEnd={`url(#flow-arrow-${edge.kind === 'analysis' ? 'analysis' : edge.kind === 'control' ? 'control' : 'audio'})`}/><title>{`${positioned.get(edge.source)?.label} → ${positioned.get(edge.target)?.label}${edge.label ? ` · ${edge.label}` : ''}`}</title></g>)}</svg>
          {layout.nodes.map(node => {
            const nodeParameters = node.parameterIds.map(id => parameterMap.get(id)).filter((parameter): parameter is Parameter => !!parameter);
            const nodeChoices = (node.choiceIds || []).map(id => choiceMap.get(id)).filter((choice): choice is Choice => !!choice);
            const compact = node.signal.startsWith('key.') && !nodeChoices.length && !nodeParameters.length;
            return <article key={node.id} className={`flow-node ${node.kind} ${compact ? 'event-node' : ''} ${node.id === graph.output ? 'output-node' : ''} ${selected === node.id ? 'selected' : ''} ${selected && !active.has(node.id) ? 'dimmed' : ''}`} data-node-id={node.id} data-signal={node.signal}
              style={{ left: node.x, top: node.y, width: node.width, height: node.height, '--flow-colour': colour(node.kind) } as CSSProperties}>
              <span className="flow-port input" style={{ top: `calc(${Math.min(node.height / 2, 68 * (props.fontScale ?? 1))}px - 0.083333rem)` }}/><span className="flow-port output" style={{ top: `calc(${Math.min(node.height / 2, 68 * (props.fontScale ?? 1))}px - 0.083333rem)` }}/>
              <button className="flow-node-title" onClick={() => select(node)} title={`Reveal ${node.label} in code`}><ModuleGlyph op={node.op}/><span><small>{node.id === graph.output ? 'INSTRUMENT OUT' : opLabel[node.op] || node.op.replace(/_/g, ' ').toUpperCase()}</small><strong>{node.label === '$output' ? 'Output' : node.label}</strong></span><span className="flow-source-line">↗</span></button>
              {!compact && <><MiniSignal node={node} signal={props.signals?.find(signal => signal.name === node.signal)} documentId={props.documentId} onSelect={() => props.onObserve([node.signal])} onInspect={() => { select(node); props.onInspect(node.signal); }} error={props.observation?.error} waiting={!!props.observation?.skipped[node.signal]}/>{nodeChoices.length > 0 && choiceControls(nodeChoices)}{nodeParameters.length > 0 && <div className="flow-knobs">{nodeParameters.slice(0, 4).map(parameter => <ParameterKnob key={`${props.documentId}:${parameter.id}`} parameter={parameter} source={props.source} documentId={props.documentId} disabled={!readable} onChange={props.onChange}/>)}</div>}</>}
              <div className="flow-node-actions"><button aria-label={`Inspect ${node.signal}`} title={`Probe ${node.signal}`} onClick={() => { select(node); props.onInspect(node.signal); }}><Icon name="expand" size={12}/></button>{(props.signals?.find(signal => signal.name === node.signal)?.auditionable ?? node.kind === 'audio') && <button aria-label={props.audition === node.signal ? `Stop listening to ${node.signal}` : `Listen to ${node.signal}`} aria-pressed={props.audition === node.signal} disabled={props.listenPending} className={props.audition === node.signal ? 'listening' : ''} onClick={() => { select(node); props.onListen(node.signal, props.audition !== node.signal); }}><Icon name="headphones" size={12}/></button>}</div>
            </article>;
          })}
        </div>
        <div className="flow-minimap" aria-hidden="true"><svg viewBox={`0 0 ${layout.width} ${layout.height}`}>{layout.edges.map(edge => <path key={edge.id} d={edgePath(edge.source, edge.target)} stroke={colour(edge.kind)} fill="none" strokeWidth="3" opacity=".5"/>)}{layout.nodes.map(node => <rect key={node.id} x={node.x} y={node.y} width={node.width} height={node.height} rx="10" fill={colour(node.kind)} opacity={selected === node.id ? '.9' : '.35'}/>)}<rect className="flow-minimap-view" x={-camera.x / camera.zoom} y={-camera.y / camera.zoom} width={viewportSize.width / camera.zoom} height={viewportSize.height / camera.zoom}/></svg></div>
        {!readable && <div className="flow-pending">{props.status === 'error' ? 'Showing last compiled flow' : props.status === 'draft' ? 'Editing source' : 'Updating flow'}<span>Parameter controls resume after compilation.</span></div>}
      </>}
    </div>
    <footer className="flow-status"><span className="flow-status-dot"/><span>{selectedNode ? <><strong>{selectedNode.label}</strong><span className="flow-path-hint"> · signal path highlighted</span></> : 'Select a signal to trace its path'}</span><div className="spacer"/>{props.observation?.error && <span className="flow-capture-budget capture-error" role="status">{props.observation.error}</span>}{selected && <button onClick={clearSelection}>Clear focus</button>}<span className="flow-gesture-hint">Drag to pan · Pinch to zoom · F to fit</span></footer>
    {selectedNode && (allSelectedParameters.length > 4 || allSelectedChoices.length > 0) && <aside className="flow-parameter-drawer" aria-label={`${selectedNode.label} parameters`}><header><strong>{selectedNode.label}</strong><span>Parameters · changes update code</span><button onClick={clearSelection} title="Close parameters"><Icon name="close" size={15}/></button></header>{allSelectedChoices.length > 0 && choiceControls(allSelectedChoices, true)}{allSelectedParameters.length > 0 && <div className="flow-detail-knobs">{allSelectedParameters.map(parameter => <ParameterKnob key={`${props.documentId}:${parameter.id}`} parameter={parameter} source={props.source} documentId={props.documentId} disabled={!readable} onChange={props.onChange} large/>)}</div>}</aside>}
  </section>;
}
