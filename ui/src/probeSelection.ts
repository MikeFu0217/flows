import type { Signal, Telemetry, Projection } from './types';

// Several analysis views may share one captured DSP signal. The native compiler
// provides these roots; counting visible names or graph cards is not the budget.
export function planObservation(signals: Signal[], priority: string[] = [], budget = 8, viewLimit = 128) {
  const byName = new Map(signals.map(signal => [signal.name, signal]));
  const order = [...new Set([...priority, ...signals.filter(signal => signal.kind === 'analysis').map(signal => signal.name), ...byName.keys()])];
  const roots = new Set<number | string>(), selected: string[] = [], skipped: Record<string, string> = {};
  for (const name of order) {
    const signal = byName.get(name); if (!signal) continue;
    if (selected.length >= viewLimit) {
      skipped[name] = `Not displayed: the ${viewLimit}-view limit is in use. Select this signal to prioritise it.`;
      continue;
    }
    // A previous host may not supply metadata; its own validation remains
    // authoritative, and any rejection is displayed by the inspector.
    const needed = new Set<number | string>(signal.captureRoots ?? [`signal:${name}`]);
    const combined = new Set([...roots, ...needed]);
    if (combined.size > budget) {
      skipped[name] = needed.size > budget
        ? `This view needs ${needed.size} captured inputs; the limit is ${budget}.`
        : `Not captured: the ${budget}-input observation budget is in use. Select this signal to prioritise it.`;
      continue;
    }
    selected.push(name); for (const root of needed) roots.add(root);
  }
  return { signals: selected.sort(), roots: roots.size, skipped };
}

export function frameForObservationVoice(frame: Telemetry, voice: number, projection: Projection = 'mid'): Telemetry {
  return (frame.captureVoice !== undefined && frame.captureVoice !== voice) || (frame.captureProjection !== undefined && frame.captureProjection !== projection)
    ? { ...frame, signals: [], error: undefined, gap: true, status: 'Waiting for selected voice and channel', observationEpoch: -1 }
    : frame;
}

// The native analyzer also limits transform/node work. If an automatic group
// exceeds that budget, keep the requested view usable without retrying forever.
export async function subscribeObservation<T>(send: (signals: string[]) => Promise<T>, signals: string[], focus?: string) {
  try { return { result: await send(signals), signals }; }
  catch (error) {
    if (!focus || signals.length === 1 && signals[0] === focus) throw error;
    return { result: await send([focus]), signals: [focus], reduced: error instanceof Error ? error.message : String(error) };
  }
}
