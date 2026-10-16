# 03 — How a signal becomes sound

The engine receives a prepared `Program`, note events, parameter updates, and an output buffer. It does not parse Flows text. Its job is to advance the instrument by an exact number of samples.

Read this chapter alongside [engine.hpp](../../src/engine/engine.hpp#L13) and [engine.cpp](../../src/engine/engine.cpp#L208). Line references describe the current implementation; use the named function if later edits move a line.

## Start with the data, not the loop

`EngineConfig` supplies sample rate, maximum block size, and maximum voices. A *sample* is one value at one instant. A *block* is a group of samples supplied together by the host. Flows code does not mention blocks because changing a host's block size should not change the instrument.

The engine keeps three different kinds of storage:

| Storage | Contents | Lifetime |
| --- | --- | --- |
| `Program` | Ordered nodes, connections, parameter descriptions, completion conditions | Prepared instrument |
| `states` | Oscillator phases, envelope stages, filter and distortion memory | Independent per voice and stateful instance |
| `scratch` | Computed sample arrays for each graph node | Reused when rendering the next voice |

The constructor [checks and allocates these arrays](../../src/engine/engine.cpp#L242). Every node's input must have a smaller ID, so evaluating nodes in ID order evaluates their dependencies first. Each stateful node has exactly one state producer. Mono allocates one set of state; poly allocates one set per available voice. Parameters are prepared separately and shared across voice evaluations.

This is an interpreter of a prepared graph: `render_node` switches on `Op` and runs the corresponding C++ sample loop. There is no JIT or generated machine code. There is also no general scratch-memory reuse between nodes yet; keeping all node results makes observation straightforward.

## Follow one note

An [`Event`](../../src/engine/engine.hpp#L13) contains an absolute sample position, `note_on` or `note_off`, a `note_id`, MIDI pitch, and velocity. The ID identifies a press, not a pitch: two presses of the same pitch can coexist and release independently.

[`apply`](../../src/engine/engine.cpp#L462) records a press in the held-note ledger. Poly finds the first free voice slot, clears that slot's DSP state, and increments its generation. If all 32 prepared slots are occupied, the new note is rejected; the ledger still remembers its eventual release. There is no voice stealing. Release tails occupy slots until completion.

Mono always uses slot 0. A new press updates pitch and velocity. `each_press` produces a trigger for every press; `gate_rise` triggers only when the previous gate was low. Releasing the current mono key returns to the most recently pressed key still held. Releasing another held key does not retarget the current one.

[`make_controls`](../../src/engine/engine.cpp#L559) turns this event state into sample arrays:

- `pitch`: MIDI pitch, optionally moving through a glide.
- `gate`: 1 while the selected key is held, otherwise 0.
- `velocity`: the current key's velocity.
- `trigger`: a one-sample pulse at the start of the rendered segment after a triggering event.

Glide interpolates **MIDI pitch**, not frequency. With `N` glide samples, the pitch moves linearly from its current value to the new pitch; the next node converts it to hertz. That gives an exponential frequency trajectory.

## The sample timeline

[`process`](../../src/engine/engine.cpp#L816) validates the whole input block, clears the output, and divides rendering at event or parameter-update positions. Its simplified control flow is:

```text
for each next event/update time:
    render samples before that time
    apply parameter updates at that time
    apply note events at that time, in input order
    check immediate completion
render the remaining samples
```

This is explanatory pseudocode, not Flows syntax. The actual function preserves absolute sample positions. Parameters at a shared position precede note events. An event exactly at the next block's start belongs to that next block.

[`render`](../../src/engine/engine.cpp#L786) prepares parameter arrays once, then visits each allocated voice. For each voice it builds key controls, evaluates every node, captures subscribed results, and adds the voice output to the output channels. Capture reads the values already computed; it does not call an oscillator a second time.

Invalid timing, capacity overflow, or nonfinite final output causes a panic/reset and silence for the block. The sample clock still advances. The audio path returns a `ProcessResult` rather than throwing an exception.

## Oscillators: phase is the memory

The [basic oscillator loop](../../src/engine/engine.cpp#L694) stores phase `p` in cycles, between 0 and 1. At sample rate `Fs`, frequency `f` advances it by `d = f / Fs`:

```text
sine = sin(2πp)
p = (p + d) mod 1
```

The engine clamps oscillator frequency to `0 … 0.49 Fs`. MIDI pitch and decibel level become ordinary signal values through [explicit conversion nodes](../../src/engine/engine.cpp#L678):

```text
frequency = 440 × 2^((pitch − 69) / 12)
gain      = 10^(decibels / 20)
```

An ideal saw jumps from +1 to −1. Sampling that jump creates unwanted folded frequencies. Flows starts with `2p − 1` and subtracts the small polynomial correction in [`blep`](../../src/engine/engine.cpp#L174) near the wrap:

```text
if p < d:       x = p/d;       correction = 2x − x² − 1
if p > 1 − d:   x = (p−1)/d;   correction = x² + 2x + 1
otherwise:                     correction = 0
saw = 2p − 1 − correction
```

This reduces aliasing. It is not an ideally bandlimited oscillator.

### Unison and stereo

The [unison path](../../src/engine/engine.cpp#L577) has up to eight phase lanes. For `N > 1`, lane positions are evenly spaced from −1 to +1. The detune parameter is the edge offset in cents:

```text
lane_frequency = base_frequency × 2^(position × detune / 1200)
lane_pan       = clamp(pan + position × spread, −1, 1)
angle          = (lane_pan + 1) × π/4
left_gain      = √2 cos(angle)
right_gain     = √2 sin(angle)
```

The lane outputs are summed and divided by `N`. The `√2` factor makes a centered lane feed the original amplitude to each channel. `random` offsets starting phases using a deterministic hash of note identity, voice generation, oscillator, and lane. Repeating the same deterministic offline case repeats the result; changing the note identity can change it.

Legacy sine/saw and a bare `unison: 1` stay mono. The extended oscillator path is used when stereo controls are requested. Its phase initialization follows the voice's onset identity; a mono legato change under `gate_rise` need not restart it.

[`stereo`, `left`, `right`, `mid`, and `side`](../../src/engine/engine.cpp#L738) are simple channel operations:

```text
mid  = (left + right) / 2
side = (left − right) / 2
```

Arithmetic broadcasts a mono input to both stereo channels. Filters and distortion keep independent state for each channel. When a mono output is requested from a stereo instrument, the engine uses Mid.

## ADSR is a small state machine

[`Envelope::tick`](../../src/engine/engine.cpp#L45) moves through `idle → attack → decay → sustain → release → idle`. Attack rises to 1, decay reaches `S`, sustain follows `S`, and release reaches 0. Segments are **linear**, not exponential.

Each timed segment records its starting level, destination, elapsed samples, and duration. During a segment:

```text
level = start + (target − start) × elapsed / length
```

Seconds become sample counts by rounding `seconds × Fs`. Zero-duration stages can be crossed immediately. Attack and release durations are read when those stages begin. Sustain can change while the envelope runs; decay updates its destination from the current sustain value.

A rising gate or trigger enters Attack. `retrigger_from: current` starts from the current level; `zero` resets the level first. A falling gate enters Release. The code handles a trigger and release at the same sample without leaving a hanging envelope.

If an ADSR already controls amplitude, multiplying its output by `key.gate` cuts off Release. Use the envelope itself as the amplitude control when a tail is intended.

## LFO: the waveform and the clock are separate

[`lfo_wave`](../../src/engine/engine.cpp#L164) implements sine, triangle, rising saw, square, and sample-and-hold. Sample-and-hold hashes the cycle number to keep one deterministic value per cycle. The [LFO renderer](../../src/engine/engine.cpp#L618) decides which phase to use.

| Mode | Phase behavior |
| --- | --- |
| `free` | Initial phase is aligned to the engine sample clock; then the oscillator advances without key retrigger |
| `retrigger` | A positive trigger resets phase, delay, fade, and cycle count |
| `one_shot` | A trigger starts the run; phase stops at 1 when it reaches the end |
| `transport` | Phase is calculated from host/internal PPQ; stopped transport holds position |

For an ordinary `rate`, frequency is in hertz. `sync: q` expresses a fraction of a whole note, giving `f = BPM / (240q)`. Thus `sync: 1/4` repeats once per quarter note. Transport phase uses `PPQ / (4q)`, plus the phase offset. PPQ counts quarter notes.

`bipolar: true` keeps the waveform near −1 … +1; `false` maps it to 0 … 1. Delay emits zero, then fade multiplies the waveform by a linear ramp. Non-transport phase does not advance during the delay; transport phase continues to follow its external clock. With a nonzero starting phase, one-shot runs from that phase to the endpoint, not an extra full cycle. This is the current implementation, not a user-drawn LFO curve editor.

## Filter: keep two accumulators per channel

[`filter_tick`](../../src/engine/engine.cpp#L183) is a trapezoidal state-variable filter. It stores `ic1` and `ic2`, which summarize the previous integration state. The implementation follows the Cytomic derivation linked in the source.

For cutoff `fc` and normalized resonance `r`:

```text
g  = tan(πfc/Fs)
k  = √2 × (1 − 0.95r)
a1 = 1 / (1 + g(g + k))
a2 = g a1
a3 = g a2
v3 = input − ic2
v1 = a1 ic1 + a2 v3
v2 = ic2 + a2 ic1 + a3 v3
ic1 = 2v1 − ic1
ic2 = 2v2 − ic2
```

Lowpass returns `v2`; highpass returns `input − k v1 − v2`. Cutoff is clamped to `5 … 0.45 Fs`, resonance to `0 … 1`. Coefficients are recalculated only when cutoff or resonance changes; a modulated cutoff can still change each sample. Near-zero integrator values are cleared to avoid excessively tiny floating-point values.

## Distortion: reduce aliasing in the nonlinear step

[`distortion_tick`](../../src/engine/engine.cpp#L148) first applies drive. Its curve `f(x)` is `tanh(x)` for softclip, clamp to ±1 for hardclip, or a repeating triangular fold. Applying the curve directly can generate frequencies beyond Nyquist and fold them back.

Flows instead stores the previous driven sample and uses a first-order antiderivative method. If `F` is an integral of `f`:

```text
wet = (F(x_now) − F(x_previous)) / (x_now − x_previous)
```

This averages the curve over the interval between samples. For a very small denominator, the code evaluates `f` at their midpoint to avoid numerical cancellation. The stable softclip integral is `|x| + log(1 + exp(−2|x|)) − log(2)`; hardclip and fold use piecewise integrals in [`antiderivative`](../../src/engine/engine.cpp#L135).

Finally, `output = (input + mix × (wet − input)) × output_gain`. `mix: 0` is an exact dry path before output gain. ADAA reduces aliasing; there is no oversampling here and no claim that distortion is alias-free.

## Why a released voice is not immediately deleted

An output crossing zero is not evidence of silence: a sine crosses zero twice each cycle. Flows instead evaluates the compiler's completion expression. [`is_complete`](../../src/engine/engine.cpp#L522) and [`completion_offset`](../../src/engine/engine.cpp#L762) combine gate closed, envelope idle, and settled effect states.

An envelope followed by a filter may have ended while the filter still rings. Filter state must fall below the implementation's `1e-7` threshold when the completion graph requires it. Distortion's previous input and wet memory also participate. `all` and `any` in the completion graph express the conditions needed by the actual output expression; the compiler chapter explains their construction.

Completion can occur inside a block. Poly stops mixing at that sample and frees the slot. Mono retains its one kernel, including continuing state, so ungated mono code can still produce output. Place an amplitude control deliberately.

## Verify the claims in tests

[engine_tests.cpp](../../test/cpp/engine_tests.cpp#L175) checks an independent analytic sine, event boundaries, equal output across block partitions, independent states, and unchanged audio when capture opens. Later groups check [release timing and smoothing](../../test/cpp/engine_tests.cpp#L421), [filter and alias reduction](../../test/cpp/engine_tests.cpp#L487), and [stereo, unison, LFO, and distortion](../../test/cpp/engine_tests.cpp#L630). Heap instrumentation checks the exercised realtime paths for allocation and deletion.

These are executable checks of stated behavior. They do not establish perceptual equivalence with Serum or a hard realtime guarantee on every computer. Effects currently run per voice before the final voice sum; there is no global post-mix effect chain.

Next: [04 — The realtime session and host](04-realtime-and-host.md).
