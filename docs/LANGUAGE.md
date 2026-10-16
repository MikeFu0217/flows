# Flows Language

Flows describes an event-driven signal flow. The language hides audio buffers and per-sample loops: instrument source defines the processing relationships, and a running instance of that processing is a **voice**.

For a source-guided introduction, start with [Read an instrument as a signal flow](learn/01-language-and-signals.md).

## Events and voices

Input events produce control signals, which drive audio processing and its output. `key.pitch` is MIDI pitch (A4 = 69). `key.velocity` remains available during Release. `key.gate` represents held/released state, and `key.trigger` is a one-sample restart pulse.

`Inst.sigstream(...)` describes the processing for a voice. `.poly()` creates independent voices for note-ons; repeated notes at the same pitch have separate identities. `.mono(priority: last)` reuses one voice's state and returns to the previous held key when the newest key is released. State declared with `.make()` persists within its voice. Poly outputs are summed.

`retrigger: each_press` sends a trigger on each press; `gate_rise` sends one only when gate changes from 0 to 1. Returning to an older held key does not trigger again. An ADSR's `retrigger_from: current / zero` chooses its starting level; both use the full Attack duration. Simultaneous gate rise and trigger start one Attack. Release falls from the current envelope value, so multiplying the final output by gate as well would cut off its tail.

`glide: 100ms` moves from the current pitch to a new target over 100 ms, linearly in semitones, while notes overlap. The first note starts at its requested pitch. A new target during glide starts a fresh movement from the current value. The final release does not change the target; glide may continue during Release.

## Reference instruments

The complete blocks below exactly match `test/presets/`; compiler tests enforce that correspondence. Each can be opened and played on its own. Additional edge cases belong in automated tests.

| Instrument | Main coverage |
| --- | --- |
| [sine-reference](../test/presets/sine-reference.flows) | Minimal flow, inline poly policy, gate and velocity |
| [probe-lab](../test/presets/probe-lab.flows) | Filter, poly ADSR, classes and metadata, conditional signals, probes and analysis arithmetic |
| [mono-legato-glide](../test/presets/mono-legato-glide.flows) | Named event policy, mono, gate-rise trigger, glide, retrigger from current level |
| [trance-rolling-bass](../test/presets/trance-rolling-bass.flows) | Mid and sub layers, highpass/lowpass, filter envelope, restart on each press |
| [stereo-motion](../test/presets/stereo-motion.flows) | Tempo-synced LFO, unison, stereo routing, distortion, side power |
| [sample-one-shot](../test/presets/sample-one-shot.flows) | Imported audio, pitched playback, regions, reverse, automatic completion |
| [sample-loop](../test/presets/sample-loop.flows) | Stereo loop, ADSR release, filter/distortion, before/after probes |

### Sine reference

A minimal poly instrument. Pitch controls the oscillator, and velocity controls amplitude. With no envelope, release closes the output immediately.

```flows
import standard as std
from lib.osc import ClassicSine

Inst.sigstream(std.event.keys.poly()) as key:
    mOsc1 = ClassicSine.make()

    raw = mOsc1(pitch: key.pitch, unison: 1, level: -6db)
    return raw * key.gate * key.velocity
```

### Probe lab

A saw passes through a lowpass filter, with an ADSR preserving the release tail. Lower notes mix back one quarter of the removed signal; higher notes use the filtered signal alone. `SmoothLowpass` declares knob metadata, and `ToneFilter` wraps it with typed ports. Every poly voice has independent oscillator, filter, and envelope state.

```flows
import standard as std
from lib.osc import ClassicSaw as Saw
from lib.filt import ClassicFilt
from lib.env import ClassicADSR

class SmoothLowpass(ClassicFilt):
    def init:
        self.register_knob(cutoff, default=900hz, range=(80hz, 12khz), step=25hz, scale=log, update=live)

class ToneFilter:
    def init:
        self.register_knob(resonance, default=0.2, range=(0, 0.9), step=0.01)
    def signal(input: audio, cutoff: hz = 900hz, resonance: scalar = 0.1):
        stage = SmoothLowpass.make()
        return stage(input, mode: lowpass, cutoff: cutoff, resonance: resonance)

Inst.sigstream(std.event.keys.poly()) as key:
    osc = Saw.make()
    filt = ToneFilter.make()
    env = ClassicADSR.make()

    # DSP: compare the same voice before and after its lowpass
    raw = osc(pitch: key.pitch, semitune: 0, level: -12db)
    filtered = filt(input: raw, cutoff: 800hz)
    amp = env(
        gate: key.gate, trigger: key.trigger, retrigger_from: current,
        A: 10ms, D: 200ms, S: 0.5, R: 0.4s
    )
    removed = raw - filtered
    if key.pitch <= C3:
        signal = filtered + removed / 4
    else:
        signal = filtered

    # Observation: these expressions do not change the audio output
    p_before = probe(raw)
    p_after = probe(filtered)
    p_amp = probe(amp)
    amp_power = power(p_amp)
    diff = p_before - p_after
    spectrum_after = spec(p_after)
    mel_diff = mel_spec(p_before) - mel_spec(p_after)
    power_diff = power(p_before) - power(p_after)
    power_percent = 100 * power(p_after) / (power(p_before) + 1e-9)

    return signal * amp * key.velocity
```

`probe(signal)` observes a continuous sample window from an existing DSP signal. It can inspect audio or a control such as `amp` or `key.pitch`. `diff` subtracts samples; `mel_diff` subtracts mel bins; `power_diff` subtracts mean-square power. `power_percent` compares filtered power with input power; `1e-9` prevents division by zero during silence. These differences answer different questions.

For audio, Capture can select Mixed. For `amp_power`, select a sounding Voice (for example, Voice 1, which corresponds to internal slot 0) to examine that voice's envelope.

Probes and derived expressions run in the analysis path. They do not change the DSP output and cannot feed `return` or a DSP parameter. Assign a DSP expression to a name before probing it: `probe(raw * 2)` is not supported. Waveforms, spectra, and mel spectra must have matching shapes for elementwise arithmetic; a scalar can broadcast. `spec` and `spectrum` are synonyms.

### Mono legato and glide

A key policy can appear directly inside `Inst.sigstream(...)` or in a named variable. With overlapping keys, the ADSR keeps running while pitch moves to the new target over 100 ms. Releasing the newest key also glides back to the previous held key.

```flows
import standard as std
from lib.osc import ClassicSaw
from lib.env import ClassicADSR

voices = std.event.keys.mono(
    priority: last,
    retrigger: gate_rise,
    glide: 100ms
)

Inst.sigstream(voices) as key:
    mOsc1 = ClassicSaw.make()
    mAmpEnv = ClassicADSR.make()

    raw = mOsc1(pitch: key.pitch, level: -6db)
    amp = mAmpEnv(
        gate: key.gate,
        trigger: key.trigger,
        retrigger_from: current,
        A: 10ms, D: 100ms, S: 0.6, R: 300ms
    )
    return raw * amp * key.velocity
```

### Trance rolling bass: mid and sub

One mono voice contains two layers. A saw one octave above the played pitch passes through a highpass and a rapidly closing lowpass. A sine at the played pitch supplies the sub. Each layer has its own amplitude envelope; their sum is scaled by velocity. Each press restarts all three envelopes, while oscillator phase keeps advancing within the reused mono voice.

```flows
import standard as std
from lib.osc import ClassicSaw, ClassicSine
from lib.filt import ClassicFilt
from lib.env import ClassicADSR

Inst.sigstream(std.event.keys.mono(
    priority: last,
    retrigger: each_press,
    glide: 0ms
)) as key:
    mMidOsc = ClassicSaw.make()
    mSubOsc = ClassicSine.make()
    mMidHighpass = ClassicFilt.make()
    mMidLowpass = ClassicFilt.make()
    mFilterEnv = ClassicADSR.make()
    mMidAmpEnv = ClassicADSR.make()
    mSubAmpEnv = ClassicADSR.make()

    # Mid: a saw one octave up, highpassed and shaped by a filter envelope
    mid_raw = mMidOsc(
        pitch: key.pitch, octave: 1, unison: 1, level: -10db
    )
    filter_env = mFilterEnv(
        gate: key.gate,
        trigger: key.trigger,
        retrigger_from: zero,
        A: 1ms, D: 70ms, S: 0, R: 15ms
    )
    cutoff = 200hz + filter_env * 2.3khz
    mid_high = mMidHighpass(mid_raw, mode: highpass, cutoff: 120hz)
    mid_filtered = mMidLowpass(
        mid_high, mode: lowpass, cutoff: cutoff, resonance: 0.15
    )
    mid_amp = mMidAmpEnv(
        gate: key.gate,
        trigger: key.trigger,
        retrigger_from: zero,
        A: 2ms, D: 95ms, S: 0, R: 20ms
    )
    mid = mid_filtered * mid_amp

    # Sub: a mono sine at the played pitch, with its own amplitude envelope
    sub_raw = mSubOsc(pitch: key.pitch, unison: 1, level: -9db)
    sub_amp = mSubAmpEnv(
        gate: key.gate,
        trigger: key.trigger,
        retrigger_from: zero,
        A: 3ms, D: 110ms, S: 0, R: 15ms
    )
    sub = sub_raw * sub_amp

    return (mid + sub) * key.velocity
```

At F#1, the sub is about 46 Hz and lowpass cutoff falls from about 2.5 kHz to 200 Hz. Both amplitude envelopes have zero Sustain, so holding a key still produces a short note. An earlier release starts Release from the current value.

For a 138 BPM KBBB rhythm, leave the first sixteenth-note position of each beat for the kick and play F#1 on the remaining three positions, holding each note for 65 ms. With that note duration, the mid/sub envelopes reach zero about 85/80 ms after onset. MIDI events supply the rhythm; this instrument does not generate a kick.

### Stereo motion

Five detuned saw oscillators spread across stereo. A quarter-note LFO controls the lowpass, and soft clipping adds harmonics. The final ADSR controls the release tail. `left` and `right` split the signal, `stereo` recombines it, and `side` observes width. An LFO is a control signal and can be probed like audio.

```flows
import standard as std
from lib.osc import ClassicSaw
from lib.env import ClassicADSR
from lib.filt import ClassicFilt
from lib.mod import ClassicLFO
from lib.fx import ClassicDistortion

Inst.sigstream(std.event.keys.poly()) as key:
    osc = ClassicSaw.make()
    lfo = ClassicLFO.make()
    filt = ClassicFilt.make()
    drive = ClassicDistortion.make()
    env = ClassicADSR.make()

    # A quarter-note LFO restarts for each key trigger
    motion = lfo(
        shape: triangle, sync: 1/4, mode: retrigger,
        trigger: key.trigger, phase: 0, bipolar: false,
        delay: 0ms, fade: 20ms
    )
    cutoff = 300hz + motion * 3khz

    # Five detuned oscillators spread across the stereo field
    raw = osc(
        pitch: key.pitch, level: -15db, unison: 5,
        detune: 14, spread: 0.85, pan: 0, phase: 0, random: 0.7
    )
    filtered = filt(raw, mode: lowpass, cutoff: cutoff, resonance: 0.2)
    shaped = drive(filtered, mode: softclip, drive: 12db, mix: 0.65, output: -6db)
    amp = env(
        gate: key.gate, trigger: key.trigger, retrigger_from: current,
        A: 8ms, D: 180ms, S: 0.6, R: 300ms
    )

    # Stereo can also be split, processed and recombined in code
    signal_left = left(shaped)
    signal_right = right(shaped)
    signal = stereo(signal_left, signal_right)
    width = side(signal)
    p_before = probe(filtered)
    p_after = probe(shaped)
    color_diff = spec(p_after) - spec(p_before)
    width_power = power(probe(width))

    return signal * amp * key.velocity
```

### Sample one-shot

Each trigger starts an independent playhead. C3 plays this file at its original rate; other keys change pitch and duration together. The voice ends when playback finishes.

```flows
import standard as std
from lib.sample import ClassicSampler

Inst.sigstream(std.event.keys.poly()) as key:
    sampler = ClassicSampler.make()

    # A complete hit plays once. Note-off does not cut its tail.
    raw = sampler(
        file: "Samples/flows-kick.wav", trigger: key.trigger,
        pitch: key.pitch, root: C3, mode: one_shot,
        start: 0, end: 1, rate: 1, reverse: false, level: -6db
    )
    p_raw = probe(raw)
    return raw * key.velocity
```

### Sample loop with processing

A looping sample is an audio signal like an oscillator. An external ADSR controls its lifetime and preserves its release tail. Both examples and their original audio files are installed in the workspace.

```flows
import standard as std
from lib.sample import ClassicSampler
from lib.env import ClassicADSR
from lib.filt import ClassicFilt
from lib.fx import ClassicDistortion

Inst.sigstream(std.event.keys.poly()) as key:
    sampler = ClassicSampler.make()
    filt = ClassicFilt.make()
    drive = ClassicDistortion.make()
    env = ClassicADSR.make()

    # The loop continues through the envelope's release.
    raw = sampler(
        file: "Samples/flows-stereo.wav", trigger: key.trigger,
        pitch: key.pitch, root: C3, mode: loop,
        start: 0, end: 1, rate: 1, reverse: false, level: -9db
    )
    filtered = filt(raw, mode: lowpass, cutoff: 1800hz, resonance: 0.15)
    shaped = drive(filtered, mode: softclip, drive: 6db, mix: 0.4, output: -3db)
    amp = env(gate: key.gate, trigger: key.trigger, A: 10ms, D: 150ms, S: 0.7, R: 250ms)

    p_before = probe(raw)
    p_after = probe(shaped)
    color_diff = spec(p_after) - spec(p_before)
    return shaped * amp * key.velocity
```

## Developer waveform formulas

`from lib.dsp import Phasor` exposes an independent phase accumulator through the usual `.make()` / call convention. A fresh voice starts at phase zero. `clock(freq: pitch_hz(pitch))` returns the current cycle position in `[0,1)`, then advances and wraps. Frequency requires mono Hz and is clamped to `0..0.49 * sample_rate` at runtime. Each clock is called once and its result can be reused.

`sin(x)` and `cos(x)` take scalar radians and preserve channels. `pitch_hz(pitch)` converts mono pitch to Hz. `db_gain(level)` converts mono decibels to linear amplitude. These compiled operations do not execute Python. For example, `angle = phase * 6.283185307179586`, `base = sin(angle)`, and `harmonic = sin(angle * 2)` define two components from one phase accumulator.

A typed custom class can package that formula and expose registered controls. This defines a new oscillator; inherited built-in classes still customize metadata only. Custom knobs may request `smoothing=5ms` (0ms..1s) in `register_knob`; omitting it preserves the existing response. Developer mode is this authoring level, while Edit/Perform controls input behavior.

Custom formulas have no automatic anti-aliasing. The [formula oscillator example](../examples/ADCxGather/07-developer/01-formula-oscillator.flows) uses A3, adds its second harmonic with a `color` control, then edits the formula to use the third harmonic.

## Statements and types

| Syntax | Implemented rule |
| --- | --- |
| `import standard as std`, `from lib.osc import ClassicSaw as Saw` | Aliases are supported for standard and the built-in osc/env/filt/mod/fx/sample libraries |
| `voices = std.event.keys.poly()`, `Inst.sigstream(...) as key:` | A named top-level key policy and exactly one instrument block |
| `part = Class.make()`, `signal = part(...)` | Each stateful instance is called once; reuse its returned signal. Arguments use `name: value`; filter/distortion input can also be positional |
| `name = expression`, `return signal` | Define each signal name once; each signal block returns a DSP signal |
| `+ - * /`, `< <= > >= == !=` | Typed arithmetic and comparisons. DSP division needs a known nonzero constant denominator. Analysis supports arithmetic but not comparisons |
| `if ...: ... else: ...` | Both branches define the same new signal names with matching units. Mono broadcasts to stereo. The condition must be mono; branches contain pure assignments, with no state creation/calls, nested `if`, or `return` |
| `class Name(Builtin):`, `def init:` | Customize built-in metadata with `register_knob`: default, range, step, scale, update, smoothing |
| `class Name:`, `def signal(port: type = fallback):` | Static composition with typed ports and optional numeric or sample-path defaults; recursive composition is rejected |
| `probe`, `spec`/`spectrum`, `mel_spec`, `power` | Waveform capture, spectrum, mel spectrum, mean-square power, and analysis arithmetic |

Literals include scalars, scientific notation, `hz`/`khz`, `ms`/`s`, `db`, MIDI notes such as `C3`/`F#1`, `true`/`false`, and quoted sample paths. Port types include `scalar` (also `audio`/`control`), `pitch`, `hz`/`hertz`, `db`/`decibels`, `seconds`/`time`, `bool`/`boolean`, and static `sample` paths. Oscillator `pitch` accepts MIDI pitch or frequency; `octave` and `semitune` adjust MIDI pitch. Unit and channel checks are separate: `audio` and `control` are scalar-unit aliases, while signals carry one or two channels.

Stereo probes default to Mid = `(L + R) / 2`, with Left, Right, and Side available. Audio diff, power, spectrum, and mel spectrum use the selected projection; control signals do not. Side of mono audio is zero. Listen auditions the original stereo signal regardless of the visualization projection.

## Samples

Import `ClassicSampler` from `lib.sample`. Each `.make()` has one playhead per voice; every voice shares the same decoded file. The result is always stereo. A mono file sends identical values to Left and Right. Feed the result into existing arithmetic, filters, distortion, channel routing and probes.

| Argument | Behavior |
| --- | --- |
| `file` | Required static quoted path, such as `"Samples/kick.wav"`. Paths resolve from the workspace root, including in nested instrument folders. String aliases and typed `file: sample` class ports are supported. |
| `trigger` | Required signal. A positive trigger restarts the selected region. Use `key.trigger` for keyboard or sequencer events. |
| `pitch`, `root` | MIDI-pitch values, both defaulting to C3. At equal values the file plays at its original pitch. `pitch: key.pitch` makes it playable across keys. |
| `rate` | Positive playback multiplier, 1/64..64; defaults to 1. Pitch and rate change both frequency and duration. |
| `mode` | `one_shot` (default) plays to the end and completes; note-off alone does not truncate it. `loop` repeats until the voice completes. |
| `start`, `end` | Normalized file positions, 0..1, with start < end. End is exclusive. Region bounds are read on each trigger; edits affect the next trigger. |
| `reverse` | Constant `false` (default) or `true`. Reverse begins at the end of the selected region. |
| `level` | Linear gain or dB, as with oscillators. Defaults to unity. |
| `gate` | Optional in loop mode only. A zero gate immediately stops playback. For an ADSR release, omit it and multiply the sampler's output by the external envelope instead. |

Pitch, root, rate and gain can change during playback. File, mode and reverse are structural changes. A standalone poly loop must have a key-controlled output gate or envelope so its lifetime is defined. The mode dropdown disables choices that would invalidate that rule. Selecting a region with a discontinuous boundary can click; choose suitable loop points or shape the output with an envelope. Playback uses resampling, not pitch-preserving time stretch or automatic BPM synchronization.

Use **Explorer → Samples → +** to import audio. Select a sample to create a new sampler instrument, or drag it into a `file:` argument; dropping on an existing quoted path replaces it. Imports are copied into managed WAV files, so moving the original file does not break the instrument. Exported `.flowsinst` packages include referenced audio. Missing or invalid resources report an error and mute the affected instrument until repaired.

The portable decoder reads mono/stereo WAV (PCM 8/16/24/32-bit or float 32/64-bit) and AIFF/AIFC PCM/float. The native importer also accepts the formats provided by its installed JUCE decoders and converts them to WAV. Per-file limits are 64 MiB encoded, 128 MiB decoded, and 10 minutes; an instrument has at most 64 resources and 256 MiB decoded audio. Portable packages include at most 64 MiB of audio bytes. Absolute paths, URLs, `.` / `..` components, and links outside the workspace are rejected.

For CLI compilation/rendering, pass `--asset-root /path/to/workspace`. Without that option, resources resolve beside the source file. The C++ compiler itself only validates syntax; resource validation occurs during host or CLI preparation.

## LFO, unison, and stereo

Import `ClassicLFO` from `lib.mod`. Available shapes are `sine`, `triangle`, `saw`, `square`, and `sample_hold`; the last holds one random value per cycle. Output is normally 0..1; `bipolar: true` changes it to -1..1.

`rate: 2hz` specifies frequency. `sync: 1/4` specifies a whole-note division: one quarter-note per cycle, or `4 * sync` beats. Supply one or the other. Sync follows host BPM; standalone uses the BPM control in the interface. `rate: 0hz` freezes the cycle. Sync must be positive.

| LFO mode | Phase and trigger behavior |
| --- | --- |
| `free` (default) | Uses engine time for initial phase; ignores trigger |
| `retrigger` | Restarts from `phase` on each explicit trigger |
| `one_shot` | Runs from `phase` to the cycle end on each explicit trigger, then holds its final value |
| `transport` | Requires sync; follows host PPQ and holds when transport stops |

`phase` is a cycle position from 0 to 1. Output is zero during `delay`; `fade` then raises its amplitude from zero. Both accept 0..30 s. Retrigger and one-shot restart those timers on each trigger. To give every note its own delay/fade, use retrigger. Free mode uses global engine time rather than restarting its timers for each note. LFO control inputs must be mono.

`ClassicSine` and `ClassicSaw` accept a constant integer `unison` from 1 to 8. `detune` is in cents, placing the outer oscillators at ±detune from the center pitch. `spread` is 0..1 and `pan` is -1..1. Oscillators are averaged. `phase` sets initial cycle position; `random` is the initial phase randomization amount from 0 to 1, with 0 giving fixed phase.

`unison: 1` alone keeps the original mono oscillator path. Unison above 1, or explicitly supplying detune/spread/pan/phase/random, produces stereo. Unison, phase, and random must be constant; editing them rebuilds the program and restores held notes into the replacement engine. Detune, spread, and pan can be modulated by mono control signals.

`stereo(l, r)` combines two mono signals. `left(x)`/`right(x)` select a channel; `mid(x) = (L + R) / 2`; `side(x) = (L - R) / 2`. For mono input, left/right/mid return the input and side returns zero. Arithmetic runs per channel, broadcasting mono operands. Filter and distortion preserve the input channel count; their controls remain mono. Names such as `mid` may still be used as ordinary signal variables.

## Distortion

Import `ClassicDistortion` from `lib.fx`. Modes are `softclip` (tanh), `hardclip` (clamp to ±1), and `fold` (triangle folding). `drive` is gain before shaping, `mix` is the wet proportion from 0 to 1, and `output` is gain after mixing. Both gains accept decibels or a linear scalar.

All modes run inside each voice; there is no post-mix global effects chain yet. They use first-order antiderivative antialiasing, which compares an antiderivative across adjacent samples to reduce aliasing, and retain previous-sample state. This reduces aliasing; it does not guarantee an alias-free result. Placed after an envelope, distortion keeps the voice alive until its input is finished and its memory has settled. Placed before the final envelope, that envelope controls the ending.

## Parameter defaults

An optional port's default supplies its omitted value and its double-click reset value. Priority is registered class default → port fallback → primitive fallback. The outermost composition port owns its default; an inner primitive does not overwrite it. Probe-lab's authored `cutoff: 800hz` resets to `900hz`. Omitted resonance uses the registered `0.2`, ahead of the port fallback `0.1`.

Double-click reset in source and Signal Flow is an undoable edit. Reset preserves the actual default's precision rather than rounding it to the drag step. It preserves the written unit, converting ms/s, hz/khz, and `gain = 10^(db / 20)` where needed. For example, a default of `-18db` with an authored `level: 0.5` resets to about `0.12589254`. A default outside the effective range is rejected.

Required ports without defaults do not offer reset. Neither do defaults that cannot be represented as a finite value in the written unit, such as zero linear gain expressed in dB. Resettable values show `double-click for default` on hover.

| Primitive | Parameter | Default |
| --- | --- | --- |
| ClassicSine / ClassicSaw | `level` | `0db`, equivalent to linear gain 1 |
| ClassicSine / ClassicSaw | `octave` / `semitune` / `unison` | `0` / `0` / `1` |
| ClassicADSR | `A` / `D` / `S` / `R` | `10ms` / `100ms` / `0.6` / `200ms` |
| ClassicFilt | `cutoff` / `resonance` | `800hz` / `0.1` |
| ClassicSine / ClassicSaw | `detune` / `spread` / `pan` / `phase` / `random` | `10` cents / `1` / `0` / `0` / `1` on the stereo path |
| ClassicLFO | `shape` / `mode` / `rate` / `bipolar` | `sine` / `free` / `1hz` / `false` |
| ClassicLFO | `phase` / `delay` / `fade` | `0` / `0ms` / `0ms` |
| ClassicDistortion | `mode` / `drive` / `mix` / `output` | `softclip` / `12db` / `1` / `0db` |

## Discrete choices in the editor

Click an underlined symbolic or boolean value such as `triangle`, `lowpass`, or `false` to choose from a menu. `Alt+Enter` opens the menu at the cursor; arrows or typing select an item, Enter confirms, and Esc cancels. Signal Flow shows the same choices and can insert an omitted default argument. A selection edits source and is undoable in one step.

The compiler supplies the choices and their prerequisites. For example, retrigger LFO needs an explicit trigger and transport mode needs sync. Unavailable items explain the missing condition. Numeric parameters, including unison count, remain draggable numeric controls.

Built-in knob response follows parameter meaning: logarithmic time/frequency, level-oriented gain, and linear scalar controls. Explicit class `scale=linear` or `scale=log` overrides this response. Source units do not change drag sensitivity. Click a graph value or Alt-click a source number for exact input with compatible units; Shift provides fine dragging. See [editor interaction](../ui/README.md#interaction).
