export interface Port { name: string; type: string; detail: string; fallback?: string; options?: string[]; required?: boolean }
export interface Callable { name: string; detail: string; ports: Port[]; positional?: boolean }
const port = (name: string, type: string, detail: string, fallback?: string, options?: string[]): Port => ({ name, type, detail, fallback, options, required: fallback === undefined });
const oscillator = [
  port('pitch', 'pitch', 'MIDI pitch, note literal, or frequency'),
  port('level', 'gain', 'Oscillator gain: dB or linear scalar', '0db'),
  port('unison', 'scalar', 'Independent oscillators per voice; constant integer 1..8', '1'),
  port('octave', 'scalar', 'Pitch offset in octaves', '0'), port('semitune', 'scalar', 'Pitch offset in semitones', '0'),
  port('detune', 'scalar', 'Outer unison pitch offset in cents', '10'), port('spread', 'scalar', 'Unison stereo spread, 0..1', '1'),
  port('pan', 'scalar', 'Stereo position, -1..1', '0'), port('phase', 'scalar', 'Initial phase in cycles, 0..1', '0'),
  port('random', 'scalar', 'Initial phase randomization, 0..1', '1'),
];
export const libraries: Record<string, Record<string, Callable>> = {
  dsp: { Phasor: { name: 'Phasor', detail: 'Developer primitive; independent phase accumulator per voice', ports: [
    port('freq', 'hz', 'Cycle frequency; phase starts at zero and wraps in [0, 1)'),
  ] } },
  osc: {
    ClassicSine: { name: 'ClassicSine', detail: 'Sine oscillator; one state per voice', ports: oscillator },
    ClassicSaw: { name: 'ClassicSaw', detail: 'Band-limited saw oscillator; one state per voice', ports: oscillator },
  },
  env: { ClassicADSR: { name: 'ClassicADSR', detail: 'Attack, decay, sustain and release envelope', ports: [
    port('gate', 'scalar', 'Held/released state; drives sustain and release'), port('trigger', 'scalar', 'One-sample retrigger pulse', '0'),
    port('A', 'seconds', 'Attack time', '10ms'), port('D', 'seconds', 'Decay time', '100ms'),
    port('S', 'scalar', 'Sustain level, 0..1', '0.6'), port('R', 'seconds', 'Release time', '200ms'),
    port('retrigger_from', 'enum', 'Attack starts from current level or zero', 'current', ['current', 'zero']),
  ] } },
  filt: { ClassicFilt: { name: 'ClassicFilt', detail: 'State-variable lowpass or highpass filter', positional: true, ports: [
    port('input', 'audio', 'Mono or stereo input'), port('cutoff', 'hz', 'Cutoff frequency', '800hz'),
    port('resonance', 'scalar', 'Resonance, 0..0.99', '0.1'), port('mode', 'enum', 'Filter response', 'lowpass', ['lowpass', 'highpass']),
  ] } },
  mod: { ClassicLFO: { name: 'ClassicLFO', detail: 'Low-frequency oscillator; rate or beat sync', ports: [
    port('rate', 'hz', 'Cycle frequency; use rate or sync', '1hz'), { ...port('sync', 'scalar', 'Whole-note division instead of Hz rate, e.g. 1/4'), required: false },
    port('shape', 'enum', 'LFO waveform', 'sine', ['sine', 'triangle', 'saw', 'square', 'sample_hold']),
    port('mode', 'enum', 'retrigger/one_shot require trigger; transport requires sync', 'free', ['free', 'retrigger', 'one_shot', 'transport']),
    port('trigger', 'scalar', 'Restart pulse for retrigger or one_shot', '0'), port('phase', 'scalar', 'Initial phase, 0..1', '0'),
    port('bipolar', 'boolean', 'false: 0..1; true: -1..1', 'false', ['false', 'true']),
    port('delay', 'seconds', 'Time before modulation starts', '0ms'), port('fade', 'seconds', 'Time to reach full modulation', '0ms'),
  ] } },
  fx: { ClassicDistortion: { name: 'ClassicDistortion', detail: 'Stereo-capable waveshaping distortion', positional: true, ports: [
    port('input', 'audio', 'Mono or stereo input'), port('drive', 'gain', 'Gain before shaping', '12db'),
    port('mix', 'scalar', 'Wet proportion, 0..1', '1'), port('output', 'gain', 'Gain after mixing', '0db'),
    port('mode', 'enum', 'Waveshaping function', 'softclip', ['softclip', 'hardclip', 'fold']),
  ] } },
  sample: { ClassicSampler: { name: 'ClassicSampler', detail: 'Stereo sample playback with per-voice playheads', ports: [
    port('file', 'sample', 'Quoted workspace-relative audio path'), port('trigger', 'scalar', 'Restart the sample region'),
    port('pitch', 'pitch', 'Playback MIDI pitch', 'C3'), port('root', 'pitch', 'Original sample MIDI pitch', 'C3'),
    port('mode', 'enum', 'Finite one-shot or repeating region', 'one_shot', ['one_shot', 'loop']),
    port('gate', 'scalar', 'Optional immediate stop for loop mode only', '1'), port('rate', 'scalar', 'Playback speed multiplier', '1'),
    port('start', 'scalar', 'Region start, 0..1; latched at trigger', '0'), port('end', 'scalar', 'Region end, 0..1; latched at trigger', '1'),
    port('level', 'gain', 'Playback gain: dB or linear scalar', '0db'), port('reverse', 'boolean', 'Reverse playback; latched at trigger', 'false', ['false', 'true']),
  ] } },
};
export const keyFields: Port[] = [
  port('pitch', 'pitch', 'MIDI pitch; A4 = 69, retained during release'), port('gate', 'scalar', '1 while held; 0 after release'),
  port('velocity', 'scalar', 'Note velocity, 0..1, retained during release'), port('trigger', 'scalar', 'One-sample pulse when the policy retriggers'),
];
export const policies: Record<string, Callable> = {
  poly: { name: 'poly', detail: 'Independent oscillator, envelope and filter state per note', ports: [] },
  mono: { name: 'mono', detail: 'One shared voice; newest held key has priority', ports: [
    port('priority', 'enum', 'Held-key priority', 'last', ['last']),
    port('retrigger', 'enum', 'Every press, or only a new gate', 'each_press', ['each_press', 'gate_rise']),
    port('glide', 'seconds', 'Pitch transition time', '0ms'),
  ] },
};
export const knobRegistration: Callable = { name: 'register_knob', detail: 'Class metadata: name first, then name=value fields', ports: [
  { ...port('smoothing', 'seconds', 'Optional amplitude/parameter transition time, 0ms..1s; preserves the existing response when omitted'), required: false },
  { ...port('default', 'quantity', 'Optional default value in the port unit'), required: false }, { ...port('range', 'quantity', 'Optional (minimum, maximum) in the same unit'), required: false },
  { ...port('step', 'quantity', 'Optional positive step in the port unit'), required: false }, port('scale', 'enum', 'log requires a positive registered range', 'linear', ['linear', 'log']),
  port('update', 'enum', 'Live parameter change or graph rebuild', 'live', ['live', 'rebuild']),
] };
export const functions: Record<string, Callable> = {
  sin: { name: 'sin', detail: 'Per-channel sine of a scalar signal in radians', ports: [port('radians', 'scalar', 'Angle in radians')] },
  cos: { name: 'cos', detail: 'Per-channel cosine of a scalar signal in radians', ports: [port('radians', 'scalar', 'Angle in radians')] },
  pitch_hz: { name: 'pitch_hz', detail: 'Convert MIDI pitch or a note literal to Hz', ports: [port('pitch', 'pitch', 'MIDI pitch; A4 = 440 Hz')] },
  db_gain: { name: 'db_gain', detail: 'Convert decibels to linear amplitude gain', ports: [port('level', 'db', 'Amplitude in decibels')] },
  probe: { name: 'probe', detail: 'Observe an existing DSP signal; no inline arithmetic', ports: [port('signal', 'signal', 'An existing signal name')] },
  power: { name: 'power', detail: 'Mean-square power of a waveform probe', ports: [port('probe', 'waveform', 'Waveform probe')] },
  spec: { name: 'spec', detail: 'Frequency spectrum of a waveform probe', ports: [port('probe', 'waveform', 'Waveform probe')] },
  spectrum: { name: 'spectrum', detail: 'Alias of spec', ports: [port('probe', 'waveform', 'Waveform probe')] },
  mel_spec: { name: 'mel_spec', detail: 'Mel spectrum of a waveform probe', ports: [port('probe', 'waveform', 'Waveform probe')] },
  stereo: { name: 'stereo', detail: 'Combine two mono audio signals as left and right', ports: [port('left', 'audio', 'Left mono signal'), port('right', 'audio', 'Right mono signal')] },
  left: { name: 'left', detail: 'Extract the left channel', ports: [port('signal', 'audio', 'Audio signal')] },
  right: { name: 'right', detail: 'Extract the right channel', ports: [port('signal', 'audio', 'Audio signal')] },
  mid: { name: 'mid', detail: '(Left + Right) / 2', ports: [port('signal', 'audio', 'Audio signal')] },
  side: { name: 'side', detail: '(Left - Right) / 2', ports: [port('signal', 'audio', 'Audio signal')] },
};
export const portTypes = ['audio', 'control', 'scalar', 'pitch', 'hz', 'hertz', 'db', 'decibels', 'seconds', 'time', 'bool', 'boolean', 'sample'];
export const units = ['hz', 'khz', 'ms', 's', 'db'];
