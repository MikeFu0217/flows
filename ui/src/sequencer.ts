export interface SequencerStep { active: boolean; velocity: number; gate: number }
export interface SequencerRow { note: number; mute: boolean; volume?: number; steps: SequencerStep[] }
export type SequencerLeafSound = { kind: 'active' } | { kind: 'instrument'; path: string } | { kind: 'sample'; path: string; playback?: 'one_shot' | 'gated' };
export type SequencerSound = SequencerLeafSound | { kind: 'lanes'; lanes: SequencerLeafSound[] };
export interface SequencerPattern {
  metronome?: { mute: boolean; division: number; volume: number };
  sound?: SequencerSound;
  length: 16 | 32;
  division: 4 | 8 | 16 | 32;
  swing: number;
  direction: 'forward' | 'reverse' | 'pingpong' | 'random';
  root: number;
  scale: 'chromatic' | 'major' | 'minor' | 'pentatonic';
  transpose: number;
  rows: SequencerRow[];
}
export interface SequencerState extends SequencerPattern {
  revision: number; playing: boolean; playhead?: number;
  ownerDocumentId?: string;
  soundStatus?: 'ready' | 'pending' | 'error';
  soundError?: string;
}

const SCALES = {
  major: [0, 2, 4, 5, 7, 9, 11], minor: [0, 2, 3, 5, 7, 8, 10], pentatonic: [0, 2, 4, 7, 9],
};
// Display the same pitch emitted by StepSequencer.hpp, including downward ties
// and scale roots that wrap across the MIDI endpoints.
export function sequenceNote(note: number, pattern: Pick<SequencerPattern, 'transpose' | 'scale' | 'root'>) {
  const pitch = Math.max(0, Math.min(127, note + pattern.transpose));
  if (pattern.scale === 'chromatic') return pitch;
  for (let distance = 0; distance < 12; ++distance) for (const candidate of [pitch - distance, pitch + distance]) {
    if (candidate >= 0 && candidate <= 127 && SCALES[pattern.scale].includes((candidate - pattern.root + 132) % 12)) return candidate;
  }
  return pitch;
}

export function withLaneSound(pattern: SequencerPattern, index: number, sound: SequencerLeafSound): SequencerPattern {
  const lanes = pattern.rows.map((_, i): SequencerLeafSound => pattern.sound?.kind === 'lanes' ? pattern.sound.lanes[i] ?? { kind: 'active' } : pattern.sound ?? { kind: 'active' });
  return { ...pattern, sound: { kind: 'lanes', lanes: lanes.map((value, i) => i === index ? sound : value) },
    rows: pattern.rows.map((value, i) => i === index && sound.kind === 'sample' && (lanes[i].kind !== 'sample' || (lanes[i] as { path: string }).path !== sound.path) ? { ...value, note: 48 } : value) };
}
