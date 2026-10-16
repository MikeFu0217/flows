import type { Telemetry, TelemetrySignal } from './types';

const empty = (): Telemetry => ({ sequence: -1, signals: [] });
const waiting = (message?: string) => !message || /^(waiting|capture gap|transition)/i.test(message);
const valid = (signal: TelemetrySignal) => signal.valid !== false && !signal.error &&
  (signal.shape === 'scalar' ? Number.isFinite(signal.scalar ?? signal.value) : !!signal.values?.length || !!signal.spectrum?.length);
const boundary = (frame: Telemetry) => JSON.stringify([frame.programRevision, frame.generation, frame.parameterEpoch, frame.observationEpoch, frame.captureMode, frame.captureMode === 'key' ? frame.keyEpoch : 0]);

export function beatScopeAvailable(frame: Telemetry, signal?: TelemetrySignal): boolean {
  return frame.captureMode === 'transport' && !frame.captureIdle && !!signal?.beatAvailable;
}

export function captureState(frame: Telemetry, signal?: TelemetrySignal, spectrum = false): 'idle' | 'waiting' | 'held' | 'live' {
  if (frame.captureIdle) return 'idle';
  if (!signal || !valid(signal)) return 'waiting';
  const spectral = signal.shape === 'spectrum' || signal.shape === 'mel_spectrum';
  if (signal.shape !== 'scalar' && !(spectrum ? (signal.spectrum ?? (spectral ? signal.values : undefined))?.length : signal.values?.length)) return 'waiting';
  return frame.frozen || frame.captureHeld || signal.held || spectrum && signal.spectrumHeld ? 'held' : 'live';
}
export const heldLabel = (frame: Telemetry) => frame.captureHeld && frame.captureMode === 'key' && !frame.frozen ? 'LAST KEY' : 'HELD';

// Presentation state only. Retained frames are explicitly marked HELD; they are
// never fed into FFT/diff/power or combined with newer samples.
export class Observation {
  live = empty();
  private display = empty();
  private frozen?: Telemetry;
  private expectedRevision = 0;
  get frame(): Telemetry { return this.frozen ? { ...this.frozen, frozen: true } : this.display; }
  get paused() { return !!this.frozen; }
  reset(expectedRevision = 0) {
    this.live = empty(); this.display = empty(); this.frozen = undefined; this.expectedRevision = expectedRevision;
  }
  pause(value: boolean) { this.frozen = value ? this.display : undefined; }
  accept(next: Telemetry): boolean {
    if (!next || !Array.isArray(next.signals) || (next.programRevision ?? 0) < this.expectedRevision) return false;
    if (next.programRevision === this.live.programRevision && next.sequence <= this.live.sequence) return false;
    const same = boundary(next) === boundary(this.live);
    if (!same) this.frozen = undefined;
    // Natural performance idle clears live history, while an explicitly held
    // frame stays held until Resume. Transport stops still clear shared Hold.
    // No idle frame may resurrect a trace through capture-gap fallback.
    if (next.captureIdle) {
      if (next.captureMode !== 'performance') this.frozen = undefined;
      this.live = next;
      this.display = { ...next, signals: [] }; return true;
    }
    const old = new Map(same ? this.display.signals.map(signal => [signal.name, signal]) : []);
    const signals = next.signals.map(signal => {
      const previous = old.get(signal.name);
      if (valid(signal)) {
        // A key-triggered preview retains spectral peaks from this acquisition.
        // Keep exact waveform/scalar/spectral-analysis operands untouched.
        if (next.captureMode === 'key' && signal.shape !== 'spectrum' && signal.shape !== 'mel_spectrum' && signal.spectrum?.length) {
          const peak = previous?.spectrum?.length === signal.spectrum.length;
          return { ...signal, spectrum: signal.spectrum.map((v,i) => peak ? Math.max(v,previous!.spectrum![i]) : v),
            spectrumFrequencies: signal.spectrumFrequencies?.map((hz,i) => peak && previous!.spectrum![i] > signal.spectrum![i] ? previous!.spectrumFrequencies?.[i] ?? hz : hz), spectrumPeak: true, held: false, spectrumHeld: false };
        }
        if (signal.windowReady === false && !signal.spectrum?.length && previous?.spectrum?.length)
          return { ...signal, spectrum: previous.spectrum, spectrumFrequencies: previous.spectrumFrequencies, spectrumHeld: true, held: false };
        return { ...signal, held: false, spectrumHeld: false };
      }
      if (previous && valid(previous) && waiting(signal.error))
        return { ...previous, held: true, waiting: signal.error || next.status || 'Waiting for capture' };
      return signal;
    });
    if (next.gap && !signals.length && waiting(next.status))
      for (const previous of old.values()) if (valid(previous)) signals.push({ ...previous, held: true, waiting: next.status || 'Capture gap' });
    this.live = next;
    this.display = { ...next, signals };
    return true;
  }
}
