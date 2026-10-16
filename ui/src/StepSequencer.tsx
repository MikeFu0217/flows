import { CSSProperties, useEffect, useRef, useState } from 'react';
import { Icon } from './Icon';
import './StepSequencer.css';

import type { SequencerPattern, SequencerRow, SequencerStep, SequencerState, SequencerLeafSound } from './sequencer';
import { liveTelemetry, onTelemetry } from './telemetry';
import { sequenceNote as actualNote, withLaneSound } from './sequencer';
export type { SequencerPattern, SequencerState } from './sequencer';

export interface StepSequencerProps {
  documentId: string;
  state: SequencerState;
  bpm: number;
  hostTempo?: boolean;
  disabled?: boolean;
  canPlay?: boolean;
  instrumentName: string;
  instruments: { path: string; name?: string; kind?: string }[];
  samples: { path: string; name?: string }[];
  onChange: (pattern: SequencerPattern) => unknown | Promise<unknown>;
  onTempoChange: (bpm: number) => void;
  onPlayingChange: (playing: boolean) => void;
  onClose: () => void;
}

const NOTE_NAMES = ['C', 'C♯', 'D', 'D♯', 'E', 'F', 'F♯', 'G', 'G♯', 'A', 'A♯', 'B'];
const clamp = (value: number, min: number, max: number) => Math.max(min, Math.min(max, value));
const noteName = (note: number) => `${NOTE_NAMES[note % 12]}${Math.floor(note / 12) - 1}`;
function row(note: number, enabled: (step: number) => boolean = () => false): SequencerRow {
  return { note, mute: false, volume: 0, steps: Array.from({ length: 32 }, (_, step) => ({ active: enabled(step), velocity: .85, gate: .6 })) };
}
function settings(state: SequencerPattern): SequencerPattern {
  const { length, division, swing, direction, root, scale, transpose, rows, sound, metronome } = state;
  return { length, division, swing, direction, root, scale, transpose, rows, metronome: metronome ?? { mute: true, division: 4, volume: -12 }, sound: sound ?? { kind: 'active' } };
}
const soundValue = (sound?: SequencerLeafSound) => !sound || sound.kind === 'active' ? 'active' : `${sound.kind}:${sound.path}`;
const soundFromValue = (value: string): SequencerLeafSound => value.startsWith('instrument:') ? { kind: 'instrument', path: value.slice(11) }
  : value.startsWith('sample:') ? { kind: 'sample', path: value.slice(7), playback: 'one_shot' } : { kind: 'active' };

// A controlled number field keeps partially typed values local until committed.
function NumberField({ value, min, max, step = 1, label, disabled, suffix, onChange }: {
  value: number; min: number; max: number; step?: number; label: string; disabled?: boolean; suffix?: string; onChange: (value: number) => void;
}) {
  const [draft, setDraft] = useState(String(value));
  useEffect(() => setDraft(String(value)), [value]);
  const commit = (text: string) => {
    const number = Number(text);
    const next = text.trim() && Number.isFinite(number) ? clamp(Math.round(number / step) * step, min, max) : value;
    setDraft(String(next)); if (next !== value) onChange(next);
  };
  return <span className="sequencer-number"><input type="number" aria-label={label} value={draft} min={min} max={max} step={step} disabled={disabled}
    onChange={event => setDraft(event.target.value)} onBlur={event => commit(event.target.value)}
    onKeyDown={event => { if (event.key === 'Enter') { event.preventDefault(); event.currentTarget.blur(); } else if (event.key === 'Escape') { event.stopPropagation(); setDraft(String(value)); event.currentTarget.value = String(value); event.currentTarget.blur(); } }}/>{suffix && <small>{suffix}</small>}</span>;
}

export function StepSequencer({ documentId, state, bpm, hostTempo = false, disabled = false, canPlay = true, instrumentName, instruments, samples, onChange, onTempoChange, onPlayingChange, onClose }: StepSequencerProps) {
  const [pattern, setPattern] = useState<SequencerPattern>(() => settings(state));
  const patternRef = useRef(pattern); patternRef.current = pattern;
  const incoming = useRef(state); incoming.current = state;
  const callbacks = useRef({ onChange, onClose }); callbacks.current = { onChange, onClose };
  const [selected, setSelected] = useState({ row: 0, step: 0 });
  const [failure, setFailure] = useState('');
  const [playhead, setPlayhead] = useState(-1);
  useEffect(() => {
    const update = () => {
      const frame = liveTelemetry();
      const step = frame.sequencerDocumentId === documentId && frame.sequencerPlaying ? frame.sequencerStep ?? -1 : -1;
      setPlayhead(previous => previous === step ? previous : step);
    };
    update(); return onTelemetry(update);
  }, [documentId]);
  const [preset, setPreset] = useState('');
  const panel = useRef<HTMLElement>(null);
  const pending = useRef<SequencerPattern>();
  const writing = useRef(false), mounted = useRef(true);
  const paint = useRef<{ active: boolean; visited: Set<string> }>();
  const signature = JSON.stringify(settings(state));
  useEffect(() => {
    if (!writing.current) { const next = settings(incoming.current); patternRef.current = next; setPattern(next); }
  }, [signature]);
  useEffect(() => {
    mounted.current = true;
    const endPaint = () => { paint.current = undefined; };
    window.addEventListener('pointerup', endPaint); window.addEventListener('pointercancel', endPaint); window.addEventListener('blur', endPaint);
    panel.current?.focus({ preventScroll: true });
    return () => { mounted.current = false; pending.current = undefined; window.removeEventListener('pointerup', endPaint); window.removeEventListener('pointercancel', endPaint); window.removeEventListener('blur', endPaint); };
  }, []);
  const drain = async () => {
    if (writing.current) return;
    writing.current = true;
    try {
      while (pending.current && mounted.current) {
        const next = pending.current; pending.current = undefined;
        await callbacks.current.onChange(next);
      }
    } catch (cause) {
      pending.current = undefined;
      if (mounted.current) { setFailure(cause instanceof Error ? cause.message : String(cause)); const next = settings(incoming.current); patternRef.current = next; setPattern(next); }
    } finally { writing.current = false; }
  };
  const change = (edit: (current: SequencerPattern) => SequencerPattern) => {
    if (disabled) return;
    const next = edit(patternRef.current);
    patternRef.current = next; setPattern(next); setFailure(''); setPreset(''); pending.current = next; void drain();
  };
  const patch = (values: Partial<SequencerPattern>) => change(current => { const next = { ...current, ...values }; if (next.sound?.kind === 'lanes') next.sound = { kind: 'lanes', lanes: next.rows.map((_, i) => next.sound?.kind === 'lanes' ? next.sound.lanes[i] ?? { kind: 'active' } : { kind: 'active' }) }; return next; });
  const changeRow = (index: number, edit: (current: SequencerRow) => SequencerRow) => change(current => ({ ...current, rows: current.rows.map((entry, i) => i === index ? edit(entry) : entry) }));
  const changeStep = (rowIndex: number, stepIndex: number, values: Partial<SequencerStep>) => changeRow(rowIndex, current => ({ ...current, steps: current.steps.map((step, index) => index === stepIndex ? { ...step, ...values } : step) }));
  const applyPreset = (name: string) => {
    if (!name) return;
    const pitch = patternRef.current.rows[0]?.note ?? 30;
    const base = { ...patternRef.current, length: 16 as const, division: 16 as const, direction: 'forward' as const, swing: 0 };
    const rows = name === 'rolling' ? [row(pitch, step => step % 4 !== 0)]
      : name === 'offbeat' ? [row(pitch, step => step % 4 === 2)]
      : name === 'pulse' ? [row(pitch, step => step % 4 === 0)]
      : name === 'ascending' ? [0, 3, 7, 12].map((interval, index) => row(clamp(pitch + interval, 0, 127), step => step % 16 === index * 4))
      : name === 'chord' ? [0, 3, 7].map(interval => row(clamp(pitch + interval, 0, 127), step => step % 8 === 2))
      : [row(pitch)];
    patch({ ...base, rows }); setPreset(name); setSelected({ row: 0, step: 0 });
  };
  const selectedRow = Math.min(selected.row, pattern.rows.length - 1), selectedStep = Math.min(selected.step, pattern.length - 1);
  const activeRow = pattern.rows[selectedRow], activeStep = activeRow?.steps[selectedStep];
  const playingStep = state.playing ? playhead : -1;
  const bars = pattern.length / pattern.division;
  const laneSounds = pattern.rows.map((_, i): SequencerLeafSound => pattern.sound?.kind === 'lanes' ? pattern.sound.lanes[i] ?? { kind: 'active' } : pattern.sound ?? { kind: 'active' });
  const sound = laneSounds[selectedRow];
  const soundPending = JSON.stringify(pattern.sound) !== JSON.stringify(state.sound);
  const soundStatus = soundPending ? 'pending' : state.soundStatus ?? 'ready';
  const soundError = soundStatus === 'error' ? state.soundError || 'This sound could not be prepared.' : '';
  const soundName = sound.kind === 'active' ? instrumentName : sound.path.split(/[\\/]/).pop() || sound.path;
  const instrumentFiles = instruments.filter(file => file.path.toLowerCase().endsWith('.flows'));
  const laneOptions = (source: SequencerLeafSound) => <><option value="active">Active instrument</option><optgroup label="Instruments">{instrumentFiles.map(file => <option key={file.path} value={`instrument:${file.path}`}>{file.path}</option>)}</optgroup><optgroup label="Samples">{samples.map(file => <option key={file.path} value={`sample:${file.path}`}>{file.path}</option>)}</optgroup>{source.kind !== 'active' && !(source.kind === 'instrument' ? instrumentFiles : samples).some(file => file.path === source.path) && <option value={soundValue(source)}>{source.path} · captured</option>}</>;
  const readyToPlay = canPlay;
  const metronome = pattern.metronome ?? { mute: true, division: 4, volume: -12 };
  const setMetronome = (values: Partial<typeof metronome>) => patch({ metronome: { ...metronome, ...values } });
  return <section ref={panel} className="step-sequencer" id="step-sequencer" role="dialog" aria-label="Step sequencer" tabIndex={-1}
    onKeyDown={event => { if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); callbacks.current.onClose(); } }}>
    <header className="sequencer-header"><div className="sequencer-heading"><Icon name="steps" size={17}/><strong>Step sequencer</strong></div>
      <span className={`sequencer-sound-status ${soundStatus}`} role="status" aria-label="Sequencer sound status" title={`${soundName} · ${soundStatus === 'pending' ? 'Preparing sound…' : soundStatus === 'error' ? 'Sound unavailable' : 'Ready'}`}><i/></span>
      <span className={`sequencer-status ${state.playing ? 'running' : ''}`}><i/>{state.playing ? 'Playing' : 'Stopped'}</span>
      <button className="icon-button" title="Close sequencer · Esc" aria-label="Close sequencer" onClick={onClose}><Icon name="close" size={18}/></button></header>
    <div className="sequencer-body">
      {soundError && <p className="sequencer-failure sequencer-sound-error" role="alert">{soundError}</p>}
      <div className="sequencer-transport">
        <button className={`sequencer-play ${state.playing ? 'playing' : ''}`} disabled={disabled || (!state.playing && !readyToPlay)} aria-label={state.playing ? 'Stop sequencer' : 'Play sequencer'} title={state.playing ? 'Stop the pattern and release its notes' : readyToPlay ? `Play this pattern with ${soundName}` : 'Choose a ready sound to play'} onClick={() => onPlayingChange(!state.playing)}><Icon name={state.playing ? 'stop' : 'play'} size={17}/></button>
        <label className="sequencer-control"><span>{hostTempo ? 'DAW tempo' : 'Tempo'}</span><NumberField label="Sequencer tempo BPM" value={bpm} min={20} max={300} disabled={disabled || hostTempo} suffix="BPM" onChange={onTempoChange}/></label>
        <label className="sequencer-control"><span>Length</span><select aria-label="Pattern length" value={pattern.length} disabled={disabled} onChange={event => patch({ length: Number(event.target.value) as 16 | 32 })}><option value={16}>16 steps</option><option value={32}>32 steps</option></select></label>
        <label className="sequencer-control"><span>Rate</span><select aria-label="Step rate" value={pattern.division} disabled={disabled} onChange={event => patch({ division: Number(event.target.value) as SequencerPattern['division'] })}>{[4, 8, 16, 32].map(value => <option key={value} value={value}>1/{value}</option>)}</select></label>
        <label className="sequencer-control sequencer-presets"><select aria-label="Sequencer preset" title="Start from a pattern" value={preset} disabled={disabled} onChange={event => applyPreset(event.target.value)}><option value="">Pattern…</option><option value="rolling">Rolling bass · xBBB</option><option value="offbeat">Offbeat</option><option value="pulse">Quarter-note pulse</option><option value="ascending">Ascending notes</option><option value="chord">Chord stabs</option><option value="empty">Empty pattern</option></select></label>
      <div className={`sequencer-metronome ${metronome.mute ? 'muted' : ''}`}>
        <button className="sequencer-mute" aria-label="Metronome enabled" aria-pressed={!metronome.mute} title={metronome.mute ? 'Unmute metronome' : 'Mute metronome'} disabled={disabled} onClick={() => setMetronome({ mute: !metronome.mute })}><Icon name="metronome" size={17}/></button><label className="sequencer-control"><select aria-label="Metronome interval" title="Metronome interval · transport also runs with every lane muted" value={metronome.division} disabled={disabled} onChange={event => setMetronome({ division: Number(event.target.value) })}>{[[1,'4 beats'],[2,'2 beats'],[4,'1 beat'],[8,'1/2 beat'],[16,'1/4 beat'],[32,'1/8 beat']].map(([value,label]) => <option key={value} value={value}>{label}</option>)}</select></label>
        <label className="lane-volume"><span>{metronome.volume} dB</span><input aria-label="Metronome volume" type="range" min={-60} max={6} step={1} value={metronome.volume} disabled={disabled} onChange={event => setMetronome({ volume: Number(event.target.value) })}/></label>
      </div>
      </div>
      <div className="sequencer-grid-wrap"><div className="sequencer-grid" style={{ '--steps': pattern.length } as CSSProperties}>
        <div className="sequencer-ruler"><span className="sequencer-lane-label">NOTE LANES <small>{pattern.rows.length}/8</small></span><div className="sequencer-steps">{Array.from({ length: pattern.length }, (_, index) => <span key={index} className={`${index % 4 === 0 ? 'beat-start' : ''} ${playingStep === index ? 'current' : ''}`}>{index + 1}</span>)}</div></div>
        {pattern.rows.map((entry, rowIndex) => <div key={rowIndex} className={`sequencer-lane ${entry.mute ? 'muted' : ''}`}>
          <div className="sequencer-lane-controls"><select className="lane-source" aria-label={`Lane ${rowIndex + 1} sound source`} title={`Lane ${rowIndex + 1} · ${laneSounds[rowIndex].kind === 'active' ? instrumentName : (laneSounds[rowIndex] as { path: string }).path}`} value={soundValue(laneSounds[rowIndex])} disabled={disabled} onChange={event => change(current => withLaneSound(current, rowIndex, soundFromValue(event.target.value)))}>{laneOptions(laneSounds[rowIndex])}</select><button className="sequencer-mute" disabled={disabled} aria-label={`Lane ${rowIndex + 1} enabled`} aria-pressed={!entry.mute} title={`${entry.mute ? 'Unmute' : 'Mute'} note lane`} onClick={() => changeRow(rowIndex, current => ({ ...current, mute: !current.mute }))}><Icon name={entry.mute ? 'muted' : 'sound'} size={14}/></button>
            <select aria-label={`Lane ${rowIndex + 1} note`} title={`Lane pitch · output after transpose and scale: ${noteName(actualNote(entry.note, pattern))}`} value={entry.note} disabled={disabled} onChange={event => changeRow(rowIndex, current => ({ ...current, note: Number(event.target.value) }))}>{Array.from({ length: 128 }, (_, note) => <option key={note} value={note}>{noteName(note)}</option>)}</select>

          <label className="lane-volume"><span>{entry.volume ?? 0} dB</span><input aria-label={`Lane ${rowIndex + 1} volume`} type="range" min={-60} max={6} step={1} value={entry.volume ?? 0} disabled={disabled} onChange={event => changeRow(rowIndex, current => ({ ...current, volume: Number(event.target.value) }))}/></label>
            <button className="icon-button sequencer-remove" disabled={disabled || pattern.rows.length === 1} title={`Remove lane ${rowIndex + 1}`} aria-label={`Remove lane ${rowIndex + 1}`} onClick={() => { patch({ rows: pattern.rows.filter((_, index) => index !== rowIndex), sound: { kind: 'lanes', lanes: laneSounds.filter((_, index) => index !== rowIndex) } }); setSelected(current => ({ ...current, row: Math.max(0, current.row - (current.row >= rowIndex ? 1 : 0)) })); }}><Icon name="close" size={13}/></button></div>

          <div className="sequencer-steps">{entry.steps.slice(0, pattern.length).map((step, stepIndex) => <button key={stepIndex} type="button" data-sequencer-step={`${rowIndex}:${stepIndex}`} disabled={disabled}
            className={`sequencer-step ${step.active ? 'active' : ''} ${Math.floor(stepIndex / 4) % 2 ? 'alternate' : ''} ${stepIndex % 4 === 0 ? 'beat-start' : ''} ${selectedRow === rowIndex && selectedStep === stepIndex ? 'selected' : ''} ${playingStep === stepIndex ? 'current' : ''}`}
            style={{ '--velocity': step.velocity } as CSSProperties} aria-label={`Lane ${rowIndex + 1}, ${noteName(entry.note)}, step ${stepIndex + 1}${step.active ? `, velocity ${Math.round(step.velocity * 100)}%, gate ${Math.round(step.gate * 100)}%` : ', off'}`} aria-pressed={step.active}
            title={`Step ${stepIndex + 1} · ${noteName(entry.note)} · ${step.active ? 'On' : 'Off'}\nClick to toggle · drag to paint · Shift-click to edit`}
            onPointerDown={event => { if (event.button !== 0) return; event.preventDefault(); event.currentTarget.focus({ preventScroll: true }); setSelected({ row: rowIndex, step: stepIndex }); if (!event.shiftKey) { const active = !step.active; paint.current = { active, visited: new Set([`${rowIndex}:${stepIndex}`]) }; changeStep(rowIndex, stepIndex, { active }); } }}
            onPointerEnter={event => { const gesture = paint.current, key = `${rowIndex}:${stepIndex}`; if (!gesture || !(event.buttons & 1) || gesture.visited.has(key)) return; gesture.visited.add(key); setSelected({ row: rowIndex, step: stepIndex }); changeStep(rowIndex, stepIndex, { active: gesture.active }); }}
            onClick={event => { if (event.detail === 0) { setSelected({ row: rowIndex, step: stepIndex }); changeStep(rowIndex, stepIndex, { active: !step.active }); } }}
            onContextMenu={event => { event.preventDefault(); setSelected({ row: rowIndex, step: stepIndex }); }}
            onKeyDown={event => { const offsets: Record<string, number[]> = { ArrowLeft: [0, -1], ArrowRight: [0, 1], ArrowUp: [-1, 0], ArrowDown: [1, 0] }; const offset = offsets[event.key]; if (offset) { event.preventDefault(); const next = { row: clamp(rowIndex + offset[0], 0, pattern.rows.length - 1), step: clamp(stepIndex + offset[1], 0, pattern.length - 1) }; setSelected(next); panel.current?.querySelector<HTMLButtonElement>(`[data-sequencer-step="${next.row}:${next.step}"]`)?.focus({ preventScroll: true }); } }}><i/></button>)}</div>
        </div>)}
      </div></div>
      <div className="sequencer-grid-footer"><button className="icon-button" aria-label="Add note lane" title="Add note lane" disabled={disabled || pattern.rows.length >= 8} onClick={() => { patch({ rows: [...pattern.rows, row(clamp((pattern.rows[pattern.rows.length - 1]?.note ?? 30) + 7, 0, 127))] }); setSelected({ row: pattern.rows.length, step: 0 }); }}><Icon name="add" size={15}/></button><span>{bars < 1 ? `${bars * 4} beats` : `${bars} bar${bars === 1 ? '' : 's'}`} in 4/4</span><button className="icon-button" aria-label="Clear steps" disabled={disabled} title="Clear steps · keep lane notes, sounds and levels" onClick={() => patch({ rows: pattern.rows.map(entry => ({ ...entry, steps: entry.steps.map(step => ({ ...step, active: false })) })) })}><Icon name="erase" size={15}/></button></div>
      {activeRow && activeStep && <div className="sequencer-step-editor"><div className="sequencer-selection" title="Lane : step · Shift-click a step to edit without toggling"><span>Step</span><strong>{selectedRow + 1}<b>:</b>{String(selectedStep + 1).padStart(2, '0')}</strong></div>
        <button className={`sequencer-enable ${activeStep.active ? 'active' : ''}`} disabled={disabled} aria-label="Selected step enabled" title={activeStep.active ? 'Disable selected step' : 'Enable selected step'} aria-pressed={activeStep.active} onClick={() => changeStep(selectedRow, selectedStep, { active: !activeStep.active })}><Icon name={activeStep.active ? 'check' : 'circle'} size={14}/></button>
        <label className="sequencer-slider"><span>Velocity <b>{Math.round(activeStep.velocity * 100)}%</b></span><input aria-label="Selected step velocity" type="range" min={0} max={100} step={1} disabled={disabled} value={Math.round(activeStep.velocity * 100)} onChange={event => changeStep(selectedRow, selectedStep, { velocity: Number(event.target.value) / 100 })}/></label>
        <label className="sequencer-slider"><span>Gate <b>{Math.round(activeStep.gate * 100)}%</b></span><input aria-label="Selected step gate" title={sound.kind === 'sample' && sound.playback !== 'gated' ? 'One-shot samples play to the end; choose Gate playback to use step length.' : 'Note length'} type="range" min={5} max={100} step={1} disabled={disabled || sound.kind === 'sample' && sound.playback !== 'gated'} value={Math.round(activeStep.gate * 100)} onChange={event => changeStep(selectedRow, selectedStep, { gate: Number(event.target.value) / 100 })}/></label>
        {sound.kind === 'sample' && <label className="sequencer-control"><select aria-label="Sample playback" title="Selected lane playback · One-shot preserves the full sample; Gate follows step length. Original speed: C3." value={sound.playback ?? 'one_shot'} disabled={disabled} onChange={event => change(current => withLaneSound(current, selectedRow, { ...sound, playback: event.target.value as 'one_shot' | 'gated' }))}><option value="one_shot">One-shot</option><option value="gated">Gate</option></select></label>}</div>}
      <div className="sequencer-key-controls">
        <label className="sequencer-control"><span>Direction</span><select aria-label="Sequence direction" value={pattern.direction} disabled={disabled} onChange={event => patch({ direction: event.target.value as SequencerPattern['direction'] })}><option value="forward">Forward</option><option value="reverse">Reverse</option><option value="pingpong">Ping-pong</option><option value="random">Random</option></select></label>
        <label className="sequencer-control"><span>Root</span><select aria-label="Scale root" value={pattern.root} disabled={disabled || pattern.scale === 'chromatic'} onChange={event => patch({ root: Number(event.target.value) })}>{NOTE_NAMES.map((name, note) => <option key={note} value={note}>{name}</option>)}</select></label>
        <label className="sequencer-control"><span>Scale</span><select aria-label="Note scale" value={pattern.scale} disabled={disabled} onChange={event => patch({ scale: event.target.value as SequencerPattern['scale'] })}><option value="chromatic">Chromatic</option><option value="major">Major</option><option value="minor">Minor</option><option value="pentatonic">Major pentatonic</option></select></label>
        <label className="sequencer-control"><span>Transpose</span><NumberField label="Transpose semitones" value={pattern.transpose} min={-24} max={24} disabled={disabled} suffix="st" onChange={transpose => patch({ transpose })}/></label>
        <label className="sequencer-slider sequencer-swing"><span>Swing <b>{Math.round(pattern.swing * 100)}%</b></span><input aria-label="Sequencer swing" type="range" min={0} max={60} step={1} disabled={disabled} value={Math.round(pattern.swing * 100)} onChange={event => patch({ swing: Number(event.target.value) / 100 })}/></label>
      </div>
      {failure && <p className="sequencer-failure" role="alert">{failure}</p>}
    </div>
  </section>;
}
