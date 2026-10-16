// Display transforms only: the analyzer's values remain untouched for probe math.
export const DB_FLOOR = -90, DB_CEILING = 6;
export const clamp = (x: number, low: number, high: number) => Math.max(low, Math.min(high, x));
export const logPosition = (hz: number, maxHz: number) => clamp(Math.log(Math.max(20, hz) / 20) / Math.log(Math.max(21, maxHz) / 20), 0, 1);
export const decibels = (value: number, power = false) => clamp((power ? 10 : 20) * Math.log10(Math.max(1e-12, Math.abs(value))), DB_FLOOR, DB_CEILING);
// A readable, symmetric linear axis for small analysis differences. Keep its
// range during a capture so amplitude changes remain visible; callers reset it
// for a new note, parameter value or observation. Never amplify numerical dust.
export function linearPlotRange(values: number[], previous = 0) {
  let peak = 0;
  for (const value of values) if (Number.isFinite(value)) peak = Math.max(peak, Math.abs(value));
  const wanted = Math.max(1e-5, peak * 1.1);
  const exponent = Math.floor(Math.log10(wanted));
  // Parsing decimal ticks avoids engine-dependent rounding in negative powers.
  // Allow a few ULPs at a tick boundary, not a visibly oversized next tick.
  const ticks = [1, 2, 5, 10].map(value => Number(`${value}e${exponent}`));
  const range = ticks.find(value => value >= wanted * (1 - 4 * Number.EPSILON))
    ?? Number(`1e${exponent + 1}`);
  return Math.max(previous, range);
}
export function follow(previous: number, target: number, seconds: number) {
  const tau = target > previous ? .065 : .38;
  return target + (previous - target) * Math.exp(-Math.max(0, seconds) / tau);
}
export function followPeak(previous: number, level: number, seconds: number, silent = false) {
  // Preserve a short peak trail while sound is present. Once every measured
  // bin is silent, settle with the main envelope instead of lingering for six
  // seconds after a note has ended. Never descend below the current level.
  return Math.max(DB_FLOOR, level, previous - Math.max(0, seconds) * (silent ? 120 : 15));
}
export function spectrumBins(values: number[], frequencies: number[], maxHz: number, count: number, power = false) {
  const result = Array<number>(count).fill(DB_FLOOR), known = Array<boolean>(count).fill(false);
  for (let i = 0; i < values.length; i++) {
    const hz = frequencies[i];
    if (!Number.isFinite(hz) || hz < 20 || hz > maxHz || !Number.isFinite(values[i])) continue;
    const at = Math.round(logPosition(hz, maxHz) * (count-1));
    result[at] = Math.max(result[at], decibels(values[i], power)); known[at] = true;
  }
  // Interpolate only between measured bins; preserve every bucket's maximum.
  let previous = -1;
  for (let i = 0; i < count; i++) if (known[i]) {
    if (previous >= 0) for (let j = previous+1; j < i; j++) result[j] = result[previous] + (result[i]-result[previous]) * (j-previous)/(i-previous);
    previous = i;
  }
  return result;
}

// Monotone cubic tangents: rounded joins through every measured knot, with no
// overshoot between adjacent samples (including sharp spectral peaks).
export function curveTangents(values: number[]) {
  const slopes = values.slice(1).map((value, i) => value - values[i]);
  return values.map((_, i) => {
    if (!i) return slopes[0] || 0;
    if (i === values.length - 1) return slopes[i - 1] || 0;
    const a = slopes[i - 1], b = slopes[i];
    return a * b <= 0 ? 0 : 2 * a * b / (a + b);
  });
}

export class BeatEnvelope {
  low: number[] = [];
  high: number[] = [];
  opacity: number[] = [];
  ages: number[] = [];
  update(low: number[], high: number[], ages: number[], seconds: number, frozen = false) {
    if (frozen && this.low.length === low.length) return;
    const blend = (from: number, to: number, tau: number) => to + (from - to) * Math.exp(-Math.max(0, seconds) / tau);
    if (this.low.length !== low.length) {
      this.low = [...low]; this.high = [...high];
      this.opacity = ages.map(age => age < 0 ? 0 : age === 0 ? .85 : .22);
      this.ages = [...ages]; return;
    }
    for (let i = 0; i < low.length; i++) {
      if (ages[i] < 0) {
        // A missing capture is a real break; never fill it with a faded old beat.
        this.opacity[i] = 0;
      } else {
        if (this.ages[i] < 0) { this.low[i] = low[i]; this.high[i] = high[i]; }
        this.low[i] = blend(this.low[i], low[i], low[i] < this.low[i] ? .018 : .12);
        this.high[i] = blend(this.high[i], high[i], high[i] > this.high[i] ? .018 : .12);
        this.opacity[i] = blend(this.opacity[i], ages[i] === 0 ? .85 : .22, .075);
      }
      this.ages[i] = ages[i];
    }
  }
}
