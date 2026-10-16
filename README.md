# Flows

**Build a sound in code. Listen, observe and compare at every step.**

Flows is an experimental synthesizer where source code defines the instrument.
Name an intermediate signal, attach a `probe`, and hear or inspect what a
processing stage actually changes. The code editor, parameter controls, Signal
Flow and AI assistant all work with the same instrument source.

[Download the macOS preview](https://github.com/flows-audio/flows/releases/latest)
· [Language and instruments](docs/LANGUAGE.md)
· [Architecture](docs/ARCHITECTURE.md)
· [Source-guided learning](docs/learn/README.md)

## Try it

The downloadable preview runs on **Apple Silicon Macs**. Unzip `Flows.app`,
move it to Applications and open it. The build targets macOS 11 or later;
runtime compatibility on older macOS releases is not yet verified. The preview
is locally signed and not notarized, so macOS may ask you to allow it in
Privacy & Security.

1. Open `examples/sine-reference.flows` from Explorer.
2. Press **Cmd+K** to enter Perform mode. Play the piano, a MIDI keyboard,
   or the computer keys **A W S E D F T G Y H U J K**. **Z/X** changes octave.
3. Drag a code number or a Signal Flow knob. Hold **Shift** for fine adjustment.
4. Click a signal capsule to inspect its waveform/spectrum or listen to that
   point in the flow. Use the step sequencer for repeating patterns.

Cmd+K returns to Edit mode. `key.` opens completion; Ctrl+Space requests it,
Tab/Enter accepts, and Escape closes. Valid unsaved source drives the sound.
Save writes the source to disk. Compile or missing-sample errors mute the
affected instrument until repaired.

**AI is optional.** Synthesis, editing and probes work without an account or
API key. To use the assistant, configure your own OpenAI-compatible endpoint
and model in its global settings. Keys use macOS Keychain. Source proposals
require Apply; the assistant does not silently replace your instrument.

## A small instrument

```flows
import standard as std
from lib.osc import ClassicSine
from lib.env import ClassicADSR

Inst.sigstream(std.event.keys.poly()) as key:
    osc = ClassicSine.make()
    env = ClassicADSR.make()
    raw = osc(pitch: key.pitch, level: -12db)
    amp = env(gate: key.gate, trigger: key.trigger,
              A: 5ms, D: 120ms, S: 0.65, R: 180ms)
    observed = probe(raw)
    return raw * amp * key.velocity
```

`raw` names the oscillator output. `probe(raw)` observes the samples already
computed by the DSP engine. More precise questions can be written as
`before - after`, `spec(before) - spec(after)`, or
`power(before) - power(after)`. Observation does not create a second sound
engine. See the [example catalogue](examples/ADCxGather/README.md) for complete
instruments and comparisons.

## What is included

- Poly/mono key policies, retrigger, glide, ADSR, sine/saw, stereo unison,
  lowpass/highpass, LFOs, distortion and pitched sample playback.
- Formula-authored oscillators using Phasor, arithmetic and sin/cos.
- Waveform, spectrum, mel and power probes, arithmetic comparisons and
  intermediate stereo audition.
- Multiple instrument tabs, split editors, completion, draggable parameters,
  enum choices, resizable panels and themes.
- A 16/32-step sequencer with per-lane instrument/sample selection, volume,
  mute, swing, scales and a metronome.
- Portable `.flowsinst` instruments, source undo, workspace recovery and
  an optional assistant with compile/render/compare tools.

## Platform status and limits

| Target | Status |
| --- | --- |
| macOS Apple Silicon standalone | Preview build available |
| macOS AU / VST3 | Build targets and automated wrapper tests; host compatibility remains limited |
| Windows / Linux compiler and DSP | Linux CI build/tests passed; Windows builds are not yet verified |
| Windows / Linux desktop app | Planned; native window, WebView and credential adapters need work |
| Browser-only instrument | Not implemented |

The language and file formats may change. Effects run per voice; there is no
global post-mix FX chain, wavetable editor or independent sample time-stretch.
Arbitrary oscillator formulas are not automatically anti-aliased. This is not
complete Serum feature coverage. Performance depends on instrument complexity,
voice count, sample rate, observations, host and device; a full device/host
acceptance matrix has not been completed.

## Build from source

The compiler/DSP build needs **CMake 3.25+, Ninja, Python 3.9+ and a C++20
compiler**:

```sh
cmake --preset dev
cmake --build --preset dev --parallel 2
ctest --preset dev
```

For the macOS app/plugins, also install the **Xcode 26+ command-line tools** and
**Node.js 22+**, then obtain the pinned JUCE source:

```sh
git clone --branch 8.0.7 --depth 1 https://github.com/juce-framework/JUCE.git ../JUCE
npm ci --prefix ui
cmake --preset app -DJUCE_DIR="$PWD/../JUCE"
cmake --build --preset app --parallel 2
ctest --preset app
open build-app/FlowsStandalone_artefacts/Release/Flows.app
```

JUCE wrapper adapters validate the expected upstream source and generate
modified copies in the build directory. Your JUCE checkout is not edited.
Building does not install AU/VST3 plugins. Bundle paths and packaging are in
[BUILDING.md](BUILDING.md); tests are explained in [test/README.md](test/README.md).

## Contribute

Bug reports, musical examples, clearer explanations and platform fixes are
welcome. Include a small `.flows` instrument and steps to reproduce when
reporting a problem. See [CONTRIBUTING.md](CONTRIBUTING.md) for building,
testing and proposing changes. See [ROADMAP.md](ROADMAP.md) for current priorities
and [THIRD_PARTY.md](THIRD_PARTY.md) for dependencies and attribution.

## License

The complete application is licensed under **AGPL-3.0-only**. The independent
compiler (`src/compiler/`) and DSP engine (`src/engine/`) remain **Apache-2.0**
under their directory licenses. Linked JUCE modules and other dependencies
retain their own licenses. See [LICENSE](LICENSE), [NOTICE](NOTICE) and
[THIRD_PARTY.md](THIRD_PARTY.md). Source for each binary release is available at
its matching public tag and in the release package.
