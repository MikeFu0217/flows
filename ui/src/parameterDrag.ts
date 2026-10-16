import type { Parameter } from './types';

const clamp = (value: number, min: number, max: number) => Math.max(min, Math.min(max, value));
const ranged = (p: Parameter) => !!p.hasRange && Number.isFinite(p.min) && Number.isFinite(p.max) && p.max > p.min;

export function parameterStep(p: Parameter, fine = false): number {
  const step = p.step && p.step > 0 ? p.step : ranged(p) ? (p.max - p.min) / 1000 : Math.max(Math.abs(p.value), 1) * .01;
  return p.discrete ? Math.max(1, Math.round(step)) : step * (fine ? .1 : 1);
}

export function normaliseParameter(value: number, p: Parameter, fine = false): number {
  if (!Number.isFinite(value)) return p.value;
  const step = parameterStep(p, fine);
  // Preserve exact endpoints, even when the declared step does not divide the range.
  if (ranged(p) && (value <= p.min || value >= p.max)) return clamp(value, p.min, p.max);
  value = Number((Math.round(value / step) * step).toPrecision(12));
  return ranged(p) ? clamp(value, p.min, p.max) : value;
}

// Allocate most level travel to audible mixing values. The very quiet tail
// remains addressable without consuming half the knob. -120 dB is NOT mute.
const dbValues = [-120, -60, -36, -12, 0, 6, 24];
const dbPositions = [0, .03, .12, .48, .75, .88, 1];
function interpolate(value: number, xs: number[], ys: number[]): number {
  let i = 0;
  while (i < xs.length - 2 && value > xs[i + 1]) i++;
  return ys[i] + (value - xs[i]) / (xs[i + 1] - xs[i]) * (ys[i + 1] - ys[i]);
}
export function parameterMapping(p: Parameter) {
  const response = p.response ?? p.scale ?? 'linear';
  const offset = p.responseOffset ?? 0;
  const encode = (v: number): number => {
    if (response === 'decibels') return interpolate(v, dbValues, dbPositions);
    if (response === 'gain') return v < 1e-6 ? -.01 + Math.max(0, v) * 10000 : interpolate(20 * Math.log10(v), dbValues, dbPositions);
    if (response === 'offset_log' && offset > 0) return Math.log1p(Math.max(0, v) / offset);
    if (response === 'log' && p.min > 0) return Math.log(Math.max(p.min, v));
    return v;
  };
  const decode = (v: number): number => {
    if (response === 'decibels') return interpolate(v, dbPositions, dbValues);
    if (response === 'gain') return v <= -.01 ? 0 : v < 0 ? (v + .01) * .0001 : Math.pow(10, interpolate(v, dbPositions, dbValues) / 20);
    if (response === 'offset_log' && offset > 0) return offset * Math.expm1(v);
    if (response === 'log' && p.min > 0) return Math.exp(v);
    return v;
  };
  const lo = encode(p.min), hi = encode(p.max);
  return {
    toPosition: (value: number) => hi > lo ? clamp((encode(value) - lo) / (hi - lo), 0, 1) : 0,
    fromPosition: (position: number) => position <= 0 ? p.min : position >= 1 ? p.max : decode(lo + position * (hi - lo)),
  };
}
export function parameterTravel(p: Parameter): number {
  // Count/tuning steps should be predictable, regardless of the full domain.
  return p.discrete || p.name === 'octave' || p.name === 'semitune'
    ? Math.max(12, (p.max - p.min) / parameterStep(p) * 12) : 240;
}
export function parameterDrag(p: Parameter) {
  const hasRange = ranged(p), mapping = parameterMapping(p);
  let position = hasRange ? mapping.toPosition(p.value) : p.value;
  let lastValue = p.value;
  return (pixels: number, fine = false) => {
    if (!Number.isFinite(pixels) || pixels === 0 || p.hasRange && p.min === p.max) return lastValue;
    position += pixels * (fine ? .1 : 1) * (hasRange ? 1 / parameterTravel(p) : Math.max(Math.abs(p.value), 1) * .01);
    if (hasRange) position = clamp(position, 0, 1);
    lastValue = normaliseParameter(hasRange ? mapping.fromPosition(position) : position, p, fine);
    return lastValue;
  };
}

// Accept exact values and compatible unit suffixes without executing expressions.
// Preserve the source unit so a precision edit never changes binding identity.
export function parameterInput(text: string, p: Parameter, suffix: string): { value: number; literal: string } | { error: string } {
  const match = text.trim().match(/^([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:e[+-]?\d+)?)\s*([a-z]*)$/i);
  if (!match) return { error: 'Enter a finite number, optionally followed by a unit.' };
  const units: Record<string, [string, number]> = { ms:['seconds',.001], s:['seconds',1], hz:['hertz',1], khz:['hertz',1000], db:['decibels',1] };
  const typed = match[2].toLowerCase(), target = suffix.toLowerCase();
  if (typed && (typed !== target && (!units[typed] || !units[target] || units[typed][0] !== units[target][0]))) return { error: 'Use a unit compatible with this parameter.' };
  const value = Number(match[1]) * (typed ? (units[typed]?.[1] ?? 1) / (units[target]?.[1] ?? 1) : 1);
  if (!Number.isFinite(value)) return { error: 'Enter a finite number.' };
  if (p.hasRange && (value < p.min || value > p.max)) return { error: `Allowed: ${p.min}–${p.max}${suffix}` };
  if (p.discrete && !Number.isInteger(value)) return { error: 'Enter a whole number.' };
  const exact = value;
  return { value: exact, literal: `${exact}${suffix}` };
}

const numberToken = /^([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)([a-zA-Z]*)/;
// Project compiler metadata onto a draft only when every change is a numeric
// literal. All intervening code and units must match exactly. Structural edits
// return null; their stale spans must never become editable controls.
export function projectParameters(original: string, source: string, parameters: Parameter[]): Parameter[] | null {
  if (original === source) return parameters;
  const projected = new Map<string, Pick<Parameter, 'from' | 'to' | 'value'>>();
  let oldAt = 0, newAt = 0;
  for (const p of [...parameters].sort((a, b) => a.from - b.from || a.to - b.to)) {
    const key = `${p.from}:${p.to}`, existing = projected.get(key);
    if (existing) continue;
    if (p.from < oldAt || p.to <= p.from || p.to > original.length) return null;
    const between = original.slice(oldAt, p.from);
    if (!source.startsWith(between, newAt)) return null;
    newAt += between.length;
    const before = original.slice(p.from, p.to), oldMatch = before.match(numberToken), match = source.slice(newAt).match(numberToken);
    if (!oldMatch || oldMatch[0] !== before || !match || match[2] !== oldMatch[2]) return null;
    const value = Number(match[1]);
    if (!Number.isFinite(value) || p.hasRange && (value < p.min || value > p.max) || p.discrete && !Number.isInteger(value)) return null;
    projected.set(key, { from: newAt, to: newAt + match[0].length, value });
    newAt += match[0].length; oldAt = p.to;
  }
  if (source.slice(newAt) !== original.slice(oldAt)) return null;
  return parameters.map(p => ({ ...p, ...projected.get(`${p.from}:${p.to}`) }));
}
