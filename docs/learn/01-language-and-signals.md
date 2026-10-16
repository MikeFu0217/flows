# 1. Read an instrument as a signal flow

Flows lets you write the relationships that produce a sound, then inspect those relationships while the instrument plays. You do not write the audio callback, allocate voices, or loop over samples. You name signals and connect operations; the compiler and engine supply the execution machinery.

This chapter explains the implemented language. [The next chapter](02-compiler.md) follows those statements into the C++ compiler. Source locations below are navigation aids for this revision; search for the named symbol if a line moves.

## Start with one complete instrument

Open [sine-reference.flows](../../test/presets/sine-reference.flows):

```flows
import standard as std
from lib.osc import ClassicSine

Inst.sigstream(std.event.keys.poly()) as key:
    mOsc1 = ClassicSine.make()

    raw = mOsc1(pitch: key.pitch, unison: 1, level: -6db)
    return raw * key.gate * key.velocity
```

`import` makes the built-in names available. These imports are resolved by the compiler; they do not load Python packages. `Inst.sigstream(...)` describes the processing for a voice. Its argument chooses how incoming key events create or update voices.

`mOsc1 = ClassicSine.make()` declares an oscillator with memory: its phase must survive from one sample to the next. `raw = mOsc1(...)` connects its inputs and names its output. `return` chooses the signal sent to the instrument output. With poly mode, the engine adds the active voices' outputs.

The final line applies two gains. `key.gate` is 1 while the note is held and 0 after release. `key.velocity` is the note's velocity. This deliberately small example has no release envelope: releasing the key stops its output immediately. Use the ADSR examples when you want a tail.

Read `raw` as “the signal at this point,” not “one floating-point number computed when the file opens.” The engine computes its samples as the instrument runs. Writing `copy = raw` gives the same signal a second name; it does not create or advance another oscillator.

Source: [`Parser::parse_instrument`, `program.cpp:628`](../../src/compiler/program.cpp#L628), [`Parser::assignment`, `program.cpp:693`](../../src/compiler/program.cpp#L693), [`Parser::instance_call`, `program.cpp:877`](../../src/compiler/program.cpp#L877).

## Events become signals

An event says that something happened at a particular sample position, such as note-on or note-off. A signal supplies a value over time. The key policy turns the event history into four signals:

| Signal | Meaning | What changes it |
| --- | --- | --- |
| `key.pitch` | MIDI pitch; A4 is 69 | Note selection, or the glide trajectory |
| `key.gate` | Held/released state, represented as 1/0 | Note-on and the relevant note-off |
| `key.velocity` | Velocity of the selected note | Note selection; it remains available during release |
| `key.trigger` | A one-sample restart pulse | The policy's trigger rule |

The compiler adds all four to the graph, including unused fields, so the editor can offer them as observation points. Gate and trigger solve different problems: gate describes a duration; trigger marks a restart. An ADSR can remain gated while receiving another trigger.

Source: [`Parser::key_value`, `program.cpp:730`](../../src/compiler/program.cpp#L730). The event scheduling and held-note implementation are explained in the runtime chapter.

## Mono and poly choose state ownership

In `std.event.keys.poly()`, a note-on allocates a voice with its own oscillator, envelope, filter, LFO, and distortion state. Two notes do not share an envelope just because they use the same source statement. Their outputs meet at the final mix.

In `std.event.keys.mono(...)`, one voice reuses its state. The implemented priority is `last`: the most recently pressed held key is selected. Releasing it returns to the previous held key. Pitch selection, envelope restart, and glide are separate choices.

This is the policy from [mono-legato-glide.flows](../../test/presets/mono-legato-glide.flows):

```text
voices = std.event.keys.mono(
    priority: last,
    retrigger: gate_rise,
    glide: 100ms
)

Inst.sigstream(voices) as key:
```

This excerpt is the opening of an instrument, not a complete file. The same policy can be written directly inside `Inst.sigstream(...)`.

| Choice | Implemented behavior |
| --- | --- |
| `retrigger: each_press` | A trigger for each new press |
| `retrigger: gate_rise` | A trigger only when gate changes from 0 to 1 |
| Release back to an older held key | Changes the selected key without a new trigger |
| `glide: 100ms` | Moves from the current pitch to the new target over 100 ms while notes overlap |

Glide is linear in semitones, not hertz. The first note starts at its requested pitch. If another target arrives during a glide, the new movement starts from the current intermediate pitch. The last note-off does not retarget the pitch, so an in-progress glide can continue during release.

An ADSR then decides what a trigger does to its amplitude: `retrigger_from: current` attacks from its present level; `zero` starts from zero. Both use the full requested Attack duration. To retain Release, multiply the output by the envelope, rather than multiplying it by `key.gate` as well.

Source: [`VoicePolicy`, `program.hpp:69`](../../src/compiler/program.hpp#L69), [`Parser::parse_policy`, `program.cpp:499`](../../src/compiler/program.cpp#L499). Poly currently accepts no policy arguments; mono supports only the options above.

## Every signal has a unit, a rate, and a channel count

These are three separate questions. A frequency can be mono and change every sample. A stereo audio signal can have the same scalar unit as a mono control signal.

| Property | Values in the compiler | Why it matters |
| --- | --- | --- |
| Unit | `scalar`, `pitch`, `hertz`, `decibels`, `seconds`, `boolean` | Rejects incompatible arithmetic and port arguments |
| Rate | `constant`, `event_held`, `sample_varying` | Describes how the signal can vary |
| Channels | 1 or 2 | Checks mono control ports and stereo audio routing |

`audio` and `control` in a class port declaration are readable aliases for `scalar`; they are not distinct unit types. Likewise, `hz`/`hertz`, `db`/`decibels`, `seconds`/`time`, and `bool`/`boolean` are accepted port spellings. “Constant” describes a graph value's source, not a promise that its editable parameter can never change.

Numeric suffixes are normalized when parsed: `2.3khz` becomes 2300 hertz, and `100ms` becomes 0.1 seconds. Note literals such as `C3` and `F#1` become MIDI pitch. An oscillator receiving pitch gets an explicit conversion:

```text
frequency_hz = 440 * 2 ^ ((pitch - 69) / 12)
linear_gain = 10 ^ (decibels / 20)
```

These equations describe C++ operations generated by the compiler; `^` is not Flows syntax.

Arithmetic follows a small, intentional rule set:

| Expression | Result |
| --- | --- |
| `key.pitch + 12` | Pitch one octave higher |
| `key.pitch - C3` | A scalar semitone interval |
| `200hz + filter_env * 2.3khz` | A cutoff signal in hertz |
| `raw * amp` | Audio multiplied by a scalar envelope |
| `raw / 4` | Audio divided by a known nonzero scalar |
| `key.pitch + 440hz` | Rejected: incompatible units |

DSP division requires a known nonzero constant denominator. Parameters that contribute to that denominator require recompilation when changed, so a drag cannot silently introduce division by zero. This restriction does not apply in the same way to the separate analysis graph.

Stereo arithmetic broadcasts a mono operand to both channels. `stereo(left_signal, right_signal)` combines two mono signals; `left`, `right`, `mid`, and `side` select a mono view. Mid is `(L + R) / 2`; Side is `(L - R) / 2`. A filter preserves its input's channels, while its cutoff and resonance inputs must be mono.

Source: [`Unit`, `Rate`, and `Node`, `program.hpp:26`](../../src/compiler/program.hpp#L26); [`quantity`, `program.cpp:181`](../../src/compiler/program.cpp#L181); [`Parser::binary`, `program.cpp:807`](../../src/compiler/program.cpp#L807); [`Parser::channel_call`, `program.cpp:774`](../../src/compiler/program.cpp#L774).

## Stateful operations are called once

An oscillator, envelope, filter, LFO, or distortion remembers something about previous samples. Flows requires one call per `.make()` instance. For example, `raw = osc(...)` may feed a filter, a mix, and a probe, but a later `again = osc(...)` is rejected. For a second independent oscillator, use a second `.make()`.

This rule removes an ambiguity: should two calls advance the same state twice, or share the first result? Flows makes sharing explicit through the returned signal. It also gives the compiler one producer for each state record.

The code is static in this sense: it describes the connections before playback. It is not a general-purpose scripting language with arbitrary loops, collections, mutation, or dynamic module loading. All imports currently name implemented built-ins from `lib.osc`, `lib.env`, `lib.filt`, `lib.mod`, or `lib.fx`.

## Reuse a processing chain with a class

There are two implemented class forms. A subclass of a built-in adjusts parameter metadata. A class with a typed `signal` method combines operations. Here is the class portion of [probe-lab.flows](../../test/presets/probe-lab.flows):

```text
class SmoothLowpass(ClassicFilt):
    def init:
        self.register_knob(cutoff, default=900hz, range=(80hz, 12khz), step=25hz, scale=log, update=live)

class ToneFilter:
    def init:
        self.register_knob(resonance, default=0.2, range=(0, 0.9), step=0.01)
    def signal(input: audio, cutoff: hz = 900hz, resonance: scalar = 0.1):
        stage = SmoothLowpass.make()
        return stage(input, mode: lowpass, cutoff: cutoff, resonance: resonance)
```

`init` here contains compiler metadata, not arbitrary executable initialization code. A built-in subclass cannot replace the built-in DSP algorithm. `ToneFilter`, however, can compose a chain in its `signal` method. The compiler expands that method for each use. The inner `stage` of an instance named `filt` becomes a state named `filt.stage`.

The metadata serves both the sound and the editor. `default` supplies an omitted argument and the double-click reset value; `range` constrains the control; `step` controls dragging; `scale=log` changes the control mapping; `update=live` permits a compatible parameter update without replacing the DSP state. The class default outranks a port fallback, which outranks a built-in fallback. Thus this resonance defaults to 0.2, not 0.1.

Classes cannot compose recursively. All ports need declared units, and the current class system does not define arbitrary symbolic enum ports. Source: [`Parser::parse_class`, `program.cpp:552`](../../src/compiler/program.cpp#L552), [`Parser::composition`, `program.cpp:1012`](../../src/compiler/program.cpp#L1012).

## A conditional selects signals

The following excerpt from probe-lab selects a mix for lower notes:

```text
removed = raw - filtered
if key.pitch <= C3:
    signal = filtered + removed / 4
else:
    signal = filtered
```

Both branches define the same new name, with compatible units. The compiler builds both pure expressions and adds a `select` node. This does not suspend an oscillator or skip a filter's state updates. Create and call stateful instances outside the branches, then select their resulting signals.

The current implementation requires `else`, permits only pure assignments in the branches, and rejects nested `if` and branch-local `return`. It does not allow reassignment of an already-defined signal. Source: [`Parser::conditional`, `program.cpp:658`](../../src/compiler/program.cpp#L658).

## Observation is another use of the same names

The processing graph answers “what produces the sound?” The analysis graph answers “what do I want to examine?” From probe-lab:

```text
p_before = probe(raw)
p_after = probe(filtered)
diff = p_before - p_after
spectrum_after = spec(p_after)
mel_diff = mel_spec(p_before) - mel_spec(p_after)
power_diff = power(p_before) - power(p_after)
power_percent = 100 * power(p_after) / (power(p_before) + 1e-9)
```

`probe` identifies an existing DSP signal for capture. `diff` subtracts corresponding samples. `spec` computes a spectrum; `mel_spec` groups spectral information on a mel frequency scale. `power` computes mean-square power over the captured window. A difference between two spectra, and the spectrum of their sample difference, answer different questions and need not look alike.

Analysis operations work on captured windows outside the realtime DSP computation. Their results cannot feed an oscillator, filter, or `return`. This boundary lets you add observations without changing the DSP structure. It does not mean observation has zero CPU or memory cost.

`probe(raw * 2)` is not supported: assign the DSP expression to a signal name first, then probe that name. `power(probe(raw))` is supported. Waveforms and spectra can combine elementwise with the same shape, or with a scalar; a waveform and a spectrum cannot be subtracted directly. Analysis comparisons are not implemented.

For a poly instrument, select whether you want the mixed audio or a particular voice. A control signal such as an envelope is most useful with a selected voice. Stereo visualization can project Mid, Left, Right, or Side; Listen still auditions the original stereo signal.

Source: [`AnalysisNode`, `program.hpp:118`](../../src/compiler/program.hpp#L118), [`Parser::analysis_call`, `program.cpp:788`](../../src/compiler/program.cpp#L788). The capture and numerical analysis implementation is covered later in this guide.

## Choose examples by the question you want to answer

| Question | Existing compact reference |
| --- | --- |
| How does a key become a sound? | [sine-reference](../../test/presets/sine-reference.flows) |
| What did the filter remove? | [probe-lab](../../test/presets/probe-lab.flows) |
| Why does this note glide without restarting its envelope? | [mono-legato-glide](../../test/presets/mono-legato-glide.flows) |
| How do mid and sub layers form a short bass note? | [trance-rolling-bass](../../test/presets/trance-rolling-bass.flows) |
| How do modulation, width, and distortion interact? | [stereo-motion](../../test/presets/stereo-motion.flows) |

For the full built-in option list, use [LANGUAGE.md](../LANGUAGE.md). For evidence of what is accepted and rejected, read `sine_regression`, `diagnostics`, `classes_and_metadata`, and `stereo_synthesis` in [compiler_tests.cpp](../../test/cpp/compiler_tests.cpp). A successful parse is only the first layer of correctness; playback, tail behavior, and observation have their own runtime tests.
