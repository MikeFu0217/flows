# Flows instrument studies

Small, playable instruments. Each `.flows` file is self-contained. The names describe what to learn; the folder numbers give a reading order. The manifest is the complete catalogue.

The **demo** files form a short route through the main ideas. Open one, play it, change one thing, and compare.

## Open the library in Flows

The installer copies the library to `~/Library/Flows/Workspace/tutorial`. In the Explorer, expand **tutorial**, then a numbered folder. Searching a folder name also finds its instruments. Clicking a tab or editor pane makes that instrument active for sound and Signal Flow.

To install another copy from the repository root:

```sh
python3 tools/install_gather_examples.py
```

The installer never replaces an existing file. It reports unchanged files and preserves edited files. For a clean second copy, pass `--target "$HOME/Library/Flows/Workspace/tutorial-copy"`.

Explorer displays nested folders and searches their instrument names.

## Four-file live route

| Order | File | Show one idea |
| --- | --- | --- |
| 1 | [Sine and key](01-signals/01-sine-and-key.flows) | A short signal flow turns key input into sound. |
| 2 | [Filter before and after](04-probe/01-filter-before-after.flows) | Listen and look at the signal on both sides of a filter. |
| 3 | [Probe difference](04-probe/02-probe-difference.flows) | Use expressions to ask more precise questions about the change. |
| 4 | [Stereo motion](06-instruments/01-stereo-motion.flows) | The same approach scales to a complete sound with modulation, width and distortion. |

For the filter pair, hold **C3** and use the same capture setting for both signals. Switching instruments releases held notes: release the key, open the next file, then replay C3 at the same velocity. These probes sit before the final amplitude envelope and velocity multiplication. Listen at either point bypasses that downstream gain; compare while holding the note, and do not confuse tap power with final output loudness. Both files produce exactly the same audio; the third file only adds analysis expressions. Drag `800hz`, or change `lowpass` with its dropdown. Undo returns the source and sound together.

In the stereo example, play a sustained note or a chord. Change LFO `shape`, then distortion `mode`. Inspect `color_diff` or `width_power` separately so the screen stays readable. Keep capture projection on **Mid** for `width_power`: `width = side(shaped)` has already extracted Side into a mono signal. Use the **Side** projection when inspecting the stereo `p_after` instead.

## Study catalogue

| Folder | Instrument | What to change or notice |
| --- | --- | --- |
| 01-signals | [01-sine-and-key](01-signals/01-sine-and-key.flows) **demo** | `make`, pitch, gate, velocity and `return`; release is immediate. |
| | [02-units-pitch-and-gain](01-signals/02-units-pitch-and-gain.flows) | `A3` plus one octave and `440hz` produce the same frequency. Retune the frequency slightly to hear beating. |
| | [03-signal-arithmetic](01-signals/03-signal-arithmetic.flows) | Mix, subtract, scale and divide signals; use an import alias. |
| 02-performance | [01-poly-and-adsr](02-performance/01-poly-and-adsr.flows) | Each note owns its envelope; releasing one does not release the others. |
| | [02-mono-legato-and-glide](02-performance/02-mono-legato-and-glide.flows) | An event-policy variable; overlapping keys retarget one voice with glide. |
| | [03-mono-retrigger](02-performance/03-mono-retrigger.flows) | `each_press` restarts the envelope. Compare `zero` and `current`. |
| 03-sound | [01-filter-envelope](03-sound/01-filter-envelope.flows) | One ADSR controls cutoff; another controls amplitude. |
| | [02-lfo-in-hertz](03-sound/02-lfo-in-hertz.flows) | Rate, phase, bipolar output, delay and fade; observe the control signal. |
| | [03-lfo-tempo-and-trigger](03-sound/03-lfo-tempo-and-trigger.flows) | `sync: 1/4` follows tempo. Compare shapes and free/retrigger/one_shot/transport modes. |
| | [04-unison-and-stereo](03-sound/04-unison-and-stereo.flows) | Detune and spread do different jobs. Inspect `side(raw)` as width changes. |
| | [05-distortion-modes](03-sound/05-distortion-modes.flows) | Compare softclip, hardclip and fold using a simple sine input. |
| | [06-stereo-routing](03-sound/06-stereo-routing.flows) | Split and rebuild left/right; reduce width through mid/side arithmetic. |
| 04-probe | [01-filter-before-after](04-probe/01-filter-before-after.flows) **demo** | Two direct probes at known points in the flow. |
| | [02-probe-difference](04-probe/02-probe-difference.flows) **demo** | Waveform difference, magnitude-spectrum difference and power difference. |
| | [03-control-and-analysis-arithmetic](04-probe/03-control-and-analysis-arithmetic.flows) | A voice-specific envelope probe, mel difference and a guarded power ratio. |
| 05-language | [01-class-knob-metadata](05-language/01-class-knob-metadata.flows) | Register default, range, step, scale and update policy for a builtin class. |
| | [02-typed-composition](05-language/02-typed-composition.flows) | Package a signal flow in a class with typed ports and defaults. |
| | [03-pure-if-and-note-ranges](05-language/03-pure-if-and-note-ranges.flows) | Branch between signal expressions by pitch; stateful calls stay outside the branches. |
| 06-instruments | [01-stereo-motion](06-instruments/01-stereo-motion.flows) **demo** | Tempo LFO, unison, filter, distortion, release and observation in one instrument. |
| | [02-rolling-bass-mid-and-sub](06-instruments/02-rolling-bass-mid-and-sub.flows) | Separate mid and sub layers, each with its own envelope. The source produces no kick. |

## Observe without losing the sound

- Click a capsule after a signal assignment to inspect it. Drag one into Signal Flow to keep it selected for observation.
- For direct audio signals or direct audio probes, **Listen** auditions that point. Turn Listen off to return to the instrument output. A computed analysis difference is a display value, not an auditionable DSP signal.
- **Mixed** sums audio voices. Choose a sounding **Voice** to analyze its envelope or another control signal. A voice slot is a runtime allocation, not a permanently assigned note.
- **Mid**, **Left**, **Right** and **Side** select the audio channel projection used for observation. Listen retains the stereo signal.
- Wait for a complete capture before comparing it. **HELD** means the display retained a capture; it does not mean the signal is still sounding. Use **Hold** when explaining a frame; **Resume** continues the displays.
- Click a discrete value such as `triangle` or `lowpass` for its dropdown. Drag a number for continuous control. The graph writes these changes back into source; it does not maintain a separate patch.

Three expressions in the difference demo deliberately answer different questions:

```flows
diff = p_before - p_after
spectrum_diff = spec(p_before) - spec(p_after)
power_diff = power(p_before) - power(p_after)
```

`diff` subtracts matching samples and includes phase differences. `spectrum_diff` subtracts magnitude bins; it is not the spectrum of `diff`. `power_diff` subtracts mean-square values over the captured window; it is not perceived loudness or `power(diff)`.

## What these examples do not promise

`free` LFO has per-voice state initialized from engine time; it is not one shared global oscillator. In this mode, delay and fade follow the engine timeline rather than restarting with each key. Use the retrigger example for a per-key start. Poly creates independent state and sums voice outputs. An amplitude envelope preserves release only when the final output uses it instead of multiplying by `key.gate` again.

The examples cover the available language and sound-building pieces. They do not claim complete Serum feature parity, arbitrary user DSP kernels, or an unlimited general-purpose language. The source is the instrument; the graph is an explanation of it.

## Reproduce the checks

From the repository root, choose a fresh output directory:

```sh
python3 test/tools/validate_gather_examples.py \
  --binary build-app/flows \
  --output test/output/adcxgather-check-2
```

The runner checks the catalogue against the actual files, compiles every instrument, recompiles each enabled alternative dropdown choice, and renders overlapping note events at 48 kHz / 64 samples and 44.1 kHz / 127 samples. It verifies finite, non-silent audio, no clipped samples in that sequence, no unfinished voices, and a nonzero side channel in the stereo examples. It also requires byte-identical audio from the two filter probe demos.

`report.json` records measured results and output hashes. These are automated correctness checks, not a claim that manual listening has been completed. A dense chord or another level edit can be louder than the tested sequence; rehearse with your recording level before the talk.

## Developer waveform demo

[07-developer/01-formula-oscillator.flows](07-developer/01-formula-oscillator.flows) defines a waveform directly from Phasor and sin. Hold A3, inspect raw, and raise color from 0 to 0.5. Release, replace `sin(angle * 2)` with `sin(angle * 3)`, and replay the same note. The extra spectral component moves from 440 to 660 Hz. Arbitrary formulas have no automatic anti-aliasing.
