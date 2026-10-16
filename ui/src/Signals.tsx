import { beatScopeAvailable, captureState, heldLabel } from './observation';
import { useEffect, useRef, useState } from 'react';
import { drawWave, onPlot, onTelemetry, telemetry, liveTelemetry, holdObservation } from './telemetry';
import { Icon } from './Icon';
import { displayKey, isSpectral, signalDisplay, onSignalDisplay, openSignalDisplayMenu, SignalDisplayMode } from './signalDisplay';
import { Signal, Projection, Telemetry, TelemetrySignal } from './types';

interface SignalViewProps { documentId?: string; projection?: Projection; onProjectionChange?: (projection: Projection) => void; name: string; signal?: Signal; large?: boolean; listening?: boolean; listenPending?: boolean; error?: string; voice?: number; voiceLimit?: number; onVoiceChange?: (voice: number) => void; onListen?: (enabled: boolean) => void; onExpand?: () => void; onClose?: () => void }
const formatValue = (value: number | undefined) => value === undefined || !Number.isFinite(value) ? '—' : Number(value.toPrecision(6)).toString();

function SignalPlot({ name, signal, frame, sample, mode, error, large, onChoose }: {
  name: string; signal?: Signal; frame: Telemetry; sample?: TelemetrySignal; mode: SignalDisplayMode | 'scalar'; error?: string; large: boolean; onChoose?: (anchor: HTMLElement) => void;
}) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const [scope, setScope] = useState<'beat' | 'detail'>('beat');
  const spectrum = mode === 'spectrum', scalar = mode === 'scalar', spectral = isSpectral(sample ?? signal ?? {});
  const kind = signal?.kind ?? sample?.kind ?? 'signal', unit = sample?.unit ?? signal?.unit;
  const values = spectrum ? sample?.spectrum ?? (spectral ? sample?.values : undefined) : sample?.values;
  const available = !!sample && sample.valid !== false && !error && (scalar ? Number.isFinite(sample.scalar ?? sample.value) : !!values?.length);
  const state = captureState(frame, sample, spectrum), held = state === 'held';
  const beatAvailable = beatScopeAvailable(frame, sample);
  const frequencies = spectral ? sample?.frequencies : sample?.spectrumFrequencies;
  const message = error || (frame.captureIdle ? 'Play a key or sequencer to capture' : frame.gap ? frame.status || 'Waiting for capture' : spectrum ? sample?.captureKind === 'last_voice_control' ? 'Select a Voice for the control spectrum' : 'Waiting for spectrum' : 'Waiting for signal');
  const label = spectrum && sample?.spectrumPeak ? 'Spectrum · note peak' : scalar ? 'Scalar' : sample?.shape === 'mel_spectrum' || signal?.shape === 'mel_spectrum' ? 'Mel spectrum' : spectrum ? 'Spectrum' : 'Waveform';
  useEffect(() => {
    const target = canvas.current; if (!target) return;
    const update = () => drawWave(target, name, kind === 'control' ? '#e3bf83' : '#bcefc4', spectrum, scope);
    const resize = () => { const rect = target.getBoundingClientRect(); target.width = Math.max(1, Math.round(rect.width * devicePixelRatio)); target.height = Math.max(1, Math.round(rect.height * devicePixelRatio)); update(); };
    const observer = new ResizeObserver(resize); observer.observe(target); const remove = onPlot(update); resize();
    return () => { observer.disconnect(); remove(); };
  }, [name, kind, spectrum, scalar, scope]);
  return <div className="signal-plot" data-view-mode={mode} data-scope-mode={!spectrum && !scalar ? scope === 'beat' && beatAvailable ? 'beat' : 'detail' : undefined} data-capture-state={error ? 'error' : state}>
    {large && <div className="signal-plot-heading"><span>{label}</span>{!spectrum && !scalar && <div className="scope-modes"><button className={scope === 'beat' && beatAvailable ? 'active' : ''} disabled={!beatAvailable} title={beatAvailable ? 'One beat, aligned to transport. Dim trace is the previous beat.' : 'Available during sequencer playback. Manual performance uses a fixed time span.'} onClick={() => setScope('beat')}>1 Beat</button><button className={scope === 'detail' || !beatAvailable ? 'active' : ''} title="Fixed time span, aligned to a rising zero crossing when available" onClick={() => setScope('detail')}>Detail</button></div>}<span className={held ? 'observation-held' : 'live-label'}>{error ? 'ERROR' : held ? heldLabel(frame) : available ? 'LIVE' : 'WAITING'}</span></div>}
    <div className={`wave-container ${scalar ? 'scalar-container' : ''}`} role={onChoose ? 'button' : undefined} tabIndex={onChoose ? 0 : undefined} aria-label={onChoose ? `Display ${name}` : undefined} title={onChoose ? 'Choose Spectrum or Waveform' : undefined} onClick={onChoose ? event => onChoose(event.currentTarget) : undefined} onKeyDown={onChoose ? event => { if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); onChoose(event.currentTarget); } } : undefined}>
      {scalar ? <div className="scalar-value">{available && <><strong>{formatValue(sample?.scalar ?? sample?.value)}</strong><span>{unit || 'scalar'}</span></>}</div> : <canvas ref={canvas} style={error ? { visibility: 'hidden' } : undefined}/>}
      {!available && <span className={`wave-empty ${error ? 'capture-error' : ''}`}>{message}</span>}
    </div>
    <footer>{scalar || spectrum ? <span>{label.toUpperCase()}{scalar && unit && <b>{unit}</b>}</span> : <><span>RMS <b>{available ? formatValue(sample?.rms) : '—'}</b></span><span>PEAK <b>{available ? formatValue(sample?.peak) : '—'}</b></span>{large && sample?.sampleCount && frame.sampleRate ? <span>{scope === 'beat' && beatAvailable ? 'Transport sync' : 'Fixed span'}</span> : null}</>}
      {large && spectrum && frequencies?.length ? <span>{formatValue(frequencies[0])}–{formatValue(frequencies[frequencies.length - 1])} Hz</span> : null}
      <div className="spacer"/>{!large && <span className={held ? 'observation-held' : 'live-label'}>{error ? 'ERROR' : held ? heldLabel(frame) : available ? 'LIVE' : 'WAITING'}</span>}
      {large && !spectrum && sample?.captureKind === 'last_voice_control' && <span>Latest voice</span>}
    </footer>
  </div>;
}

export function SignalView({ documentId = '', name, signal, large = false, listening = false, listenPending = false, error: selectionError, voice = -1, voiceLimit = 32, onVoiceChange, projection = 'mid', onProjectionChange, onListen, onExpand, onClose }: SignalViewProps) {
  const [frame, setFrame] = useState(telemetry());
  const [, refreshDisplay] = useState(0), closeMenu = useRef<() => void>();
  const sample = frame.signals.find(value => value.name === name), kind = signal?.kind ?? sample?.kind ?? 'signal';
  const descriptor = signal ?? sample ?? {}, key = displayKey(documentId, name);
  const shape = sample?.shape || signal?.shape || 'waveform';
  const scalar = shape === 'scalar', spectral = isSpectral({ shape });
  const modes: (SignalDisplayMode | 'scalar')[] = scalar ? ['scalar'] : spectral ? ['spectrum'] : large ? ['wave', 'spectrum'] : [signalDisplay(key, descriptor)];
  const error = selectionError || frame.error || (frame.gap && frame.status && !/^(waiting|capture gap|transition)/i.test(frame.status) ? frame.status : undefined) || sample?.error;
  const listeningAllowed = signal?.auditionable ?? kind === 'audio';
  useEffect(() => onTelemetry(() => setFrame(telemetry())), []);
  useEffect(() => onSignalDisplay(() => refreshDisplay(value => value + 1)), []);
  useEffect(() => () => closeMenu.current?.(), [key]);
  return <section className={`signal-view ${large ? 'large' : ''} ${modes.length === 2 ? 'dual-view' : ''} ${listening ? 'auditioning' : ''}`}>
    <header><span className={`signal-indicator ${kind}`}/><strong title={name}>{name}</strong><span className="tag">{kind.toUpperCase()}</span><div className="spacer"/>{large && onVoiceChange && <ObservationVoice value={voice} limit={voiceLimit} onChange={onVoiceChange}/>} {large && onProjectionChange && <ObservationProjection value={projection} onChange={onProjectionChange}/>} {large && <ObservationHold/>}{onListen && <button className={`icon-button listen-button ${listening ? 'selected' : ''}`} disabled={!listeningAllowed || listenPending} aria-label={listening ? `Stop listening to ${name}` : `Listen to ${name}`} aria-pressed={listening} title={!listeningAllowed ? 'Listen is available for audio signals and direct audio probes' : listening ? 'Stop audition · return to instrument output' : 'Listen to this signal'} onClick={() => onListen(!listening)}><Icon name="headphones" size={13}/>{large && <span>{listening ? 'Listening' : 'Listen'}</span>}</button>}{onExpand && <button className="icon-button" title="Expand signal" onClick={onExpand}><Icon name="expand" size={13}/></button>}{onClose && <button className="icon-button" title="Remove signal" onClick={onClose}><Icon name="close" size={13}/></button>}</header>
    {modes.map(mode => <SignalPlot key={mode} name={name} signal={signal} frame={frame} sample={sample} mode={mode} error={error} large={large} onChoose={!large && !scalar ? anchor => { closeMenu.current = openSignalDisplayMenu(anchor, key, descriptor, onExpand); } : undefined}/>)}
  </section>;
}

export function AudioReadout() {
  const [state, setState] = useState(liveTelemetry());
  useEffect(() => onTelemetry(() => setState(liveTelemetry())), []);
  return <><span className="audio-stat"><i className="meter"><i style={{ transform: `scaleX(${Math.min(1, state.outputPeak ?? 0)})` }}/></i><span className="meter-db">{state.outputPeak === undefined ? '—' : (20 * Math.log10(Math.max(1e-6, state.outputPeak))).toFixed(1)}</span> <small>dB</small></span><span className="audio-stat">{state.voices ?? '—'} <small>voices</small></span><span className="audio-stat">{state.cpu === undefined ? '—' : `${state.cpu.toFixed(1)}%`} <small>CPU</small></span></>;
}

export function ObservationHold() {
  const [held, setHeld] = useState(!!telemetry().frozen);
  useEffect(() => onTelemetry(() => setHeld(!!telemetry().frozen)), []);
  return <button className={`observation-hold ${held ? 'active' : ''}`} aria-label={held ? 'Resume observation' : 'Hold observation'} aria-pressed={held} title={held ? 'Resume live signal displays' : 'Freeze the current measured frame across all signal displays. Audio keeps playing.'} onClick={() => holdObservation(!held)}>{held ? '▶ Resume' : 'Ⅱ Hold'}</button>;
}

export function ObservationVoice({ value, limit = 32, onChange }: { value: number; limit?: number; onChange: (voice: number) => void }) {
  return <label className="observation-voice" title="Mixed sums audio voices and displays the latest control voice. Select one voice slot for polyphonic control analysis."><span>Capture</span><select aria-label="Observation voice" value={value} onChange={event => onChange(Number(event.target.value))}><option value={-1}>Mixed</option>{Array.from({ length: Math.max(1, Math.min(32, limit)) }, (_, slot) => <option key={slot} value={slot}>Voice {slot + 1}</option>)}</select></label>;
}

export function ObservationProjection({ value, onChange }: { value: Projection; onChange: (projection: Projection) => void }) {
  return <label className="observation-voice" title="Audio observation: Mid = (L+R)/2, Side = (L−R)/2. Controls are unchanged; Listen always plays the stereo signal."><select aria-label="Observation channel" value={value} onChange={event => onChange(event.target.value as Projection)}><option value="mid">Mid · L+R</option><option value="left">Left</option><option value="right">Right</option><option value="side">Side · L−R</option></select></label>;
}

export function TempoControl({ bpm, host, disabled, onChange }: { bpm: number; host: boolean; disabled: boolean; onChange: (bpm: number) => void }) {
  const [draft, setDraft] = useState(String(bpm));
  useEffect(() => setDraft(String(bpm)), [bpm]);
  return <label className="transport-tempo" title={host ? 'Tempo follows the DAW' : 'Tempo for synced LFOs and the step sequencer'}><input aria-label="Tempo BPM" type="number" min={20} max={300} step={1} value={draft} disabled={disabled || host} onChange={event => setDraft(event.target.value)} onBlur={event => { const value = Number(event.currentTarget.value); if (Number.isFinite(value) && value >= 20 && value <= 300 && !host && !disabled) { if (value !== bpm) onChange(value); } else setDraft(String(bpm)); }} onKeyDown={event => { if (event.key === 'Enter') { event.preventDefault(); event.currentTarget.blur(); } if (event.key === 'Escape') { event.preventDefault(); event.currentTarget.value = String(bpm); setDraft(String(bpm)); event.currentTarget.blur(); } }}/><small>{host ? 'DAW' : 'BPM'}</small></label>;
}
