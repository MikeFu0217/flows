import { Telemetry, TelemetrySignal, Projection } from './types';
import { Observation } from './observation';
import { frameForObservationVoice } from './probeSelection';
const observation = new Observation();
let expectedRevision = 0, expectedVoice = -1;
let expectedProjection: Projection = 'mid';
const listeners = new Set<() => void>();
const notify = () => listeners.forEach(callback => callback());
export function publishTelemetry(next: Telemetry) {
  // A voice request is acknowledged before the audio thread applies it. Keep
  // live meters, but never label the outgoing voice's samples as the new voice.
  if (observation.accept(frameForObservationVoice(next, expectedVoice, expectedProjection))) notify();
}
export function resetTelemetry(revision = 0) { expectedRevision = revision; observation.reset(revision); notify(); }
export function selectObservationVoice(voice: number) {
  if (expectedVoice === voice) return;
  expectedVoice = voice; observation.reset(expectedRevision); notify();
}
export function selectObservationProjection(projection: Projection) {
  if (expectedProjection === projection) return;
  expectedProjection = projection; observation.reset(expectedRevision); notify();
}
export function telemetry() { return observation.frame; }
export function liveTelemetry() { return observation.live; }
export function holdObservation(hold: boolean) { observation.pause(hold); notify(); }
export function signalData(name: string): TelemetrySignal | undefined { return telemetry().signals.find(signal => signal.name === name); }
export function onTelemetry(callback: () => void) { listeners.add(callback); return () => { listeners.delete(callback); }; }

export { drawSignalPlot as drawWave } from './signalPlot';

// Canvas animation is independent of React telemetry updates and never writes
// smoothed values back into the observation frame.
const plotListeners = new Set<() => void>();
let animation = 0;
function animatePlots() {
  animation = 0;
  plotListeners.forEach(draw => draw());
  if (plotListeners.size) animation = requestAnimationFrame(animatePlots);
}
export function onPlot(draw: () => void) {
  plotListeners.add(draw);
  // WKWebView may suspend rAF in an occluded native window. New captures
  // must still update its canvas and status when telemetry arrives.
  const offTelemetry = onTelemetry(draw);
  if (!animation) animation = requestAnimationFrame(animatePlots);
  return () => { offTelemetry(); plotListeners.delete(draw); if (!plotListeners.size) { cancelAnimationFrame(animation); animation = 0; } };
}
