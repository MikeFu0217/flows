# 07. Verification and changes

A program can parse correctly and still produce the wrong sound. A renderer can produce the right sound offline and still miss a device deadline. A button can look correct and send the wrong source edit. The test directories separate these questions.

## The shortest useful commands

Run these from the repository root. The `app` preset needs the configured JUCE checkout and UI dependencies described in the [main README](../../README.md).

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev

cmake --preset app
cmake --build --preset app --parallel 2
ctest --preset app
```

The [CMake file](../../CMakeLists.txt) is the executable list of test targets. The [validation reference](../../test/README.md) describes coverage and practical limits. Results apply only to the build and environment that produced them.

## Which test answers which question?

| Question | Start here |
| --- | --- |
| Does a source construct have the intended meaning? | [compiler_tests.cpp](../../test/cpp/compiler_tests.cpp) |
| Is a waveform, envelope, filter or event sequence numerically correct? | [engine_tests.cpp](../../test/cpp/engine_tests.cpp) |
| Do program changes, note replay, automation and capture preserve their contracts? | [session_tests.cpp](../../test/cpp/session_tests.cpp) |
| Are FFT, mel, power and observation resets correct? | [analyzer_tests.cpp](../../test/cpp/analyzer_tests.cpp) |
| Are document history, persistence, source spans and native requests correct? | [host_tests.cpp](../../test/cpp/host_tests.cpp) |
| Do actual AU and VST3 wrappers preserve events and stereo? | [plugin_tests.cpp](../../test/cpp/plugin_tests.cpp) |
| Does the real embedded window behave correctly? | [window_tests.mm](../../test/cpp/window_tests.mm) |
| Do small frontend rules preserve source and layout? | [ui/test](../../ui/test) |
| Do complete event fixtures render consistently across block sizes? | [run_v1_suite.py](../../test/tools/run_v1_suite.py) |

The window test opens the real WKWebView and drives the native bridge. The plugin test loads real local bundles. These checks do not cover every DAW, audio device or macOS version.

## Why block-size comparisons matter

A note event at sample 400 should start at sample 400 whether the host requests blocks of 1, 64 or 127 samples. The same principle applies to envelopes, tempo-synced modulation and parameter ramps. Rendering the same event list with different block sizes exposes accidental dependence on callback boundaries.

The regression tools compare output files and measured properties. A matching hash is strong evidence for identical rendered samples under those conditions. It says nothing about which sound a listener prefers.

## Following a defect to its owner

If `cutoff: 800hz` is rejected, begin in the compiler's argument handling and unit rules. If it compiles but the response is wrong, begin in the filter node and engine tests. If a knob writes the wrong literal, inspect the compiler span, UTF-8 to UTF-16 conversion, then the UI edit. If correct samples flash or vanish in the display, inspect capture sequence and epoch continuity before changing the DSP.

For a stuck note, track the note identity from host event through session queues into the voice. MIDI pitch alone is insufficient when the same pitch is pressed more than once. For a click during editing, establish whether the edit should have updated a parameter or rebuilt the program.

## Where a new feature would go

A new **DSP primitive** needs a compiler definition of its arguments and types, a node representation, prepared state, an engine kernel and completion behavior where relevant. It also needs numerical and lifecycle tests. Add parameter and choice metadata so the code and graph controls describe the same interface.

A new **analysis operation** needs an analysis node, type/shape rules, a worker implementation and dependency-budget accounting. It should use existing captured roots where possible. It must not enter the audio output graph merely because the UI can display it.

A new **editing control** should produce an ordinary source edit with a matching revision and a single undo gesture. It should not create a second hidden instrument model. Test stale spans, cancellation and rapid document changes.

A new **AI tool** needs a bounded, explicit host operation, cancellation and revision handling. A proposed edit should remain reviewable before application. A tool result should report what it measured, rather than claim it listened or proved a musical judgment.

## Performance claims need a workload

The audio callback uses prepared memory and bounded queues. Those choices reduce avoidable work on the realtime thread. They are architectural facts, not a guarantee that every device callback will finish before its deadline.

Record sample rate, block size, voice count, instrument, capture selection, machine and build for a benchmark. Compare processing time with the block duration. At 48 kHz with 128 samples, the nominal interval is about 2.67 ms. An average below that interval can conceal occasional misses, so inspect tail latency and actual miss counts as well.

Benchmark percentiles alone are not unconditional performance claims. Report deadline misses, callback allocation checks, instrument complexity and the tested environment together. The preview does not claim complete device or DAW performance acceptance.

## After reading the guide

You should be able to point to the code responsible for a sound, explain which state belongs to one voice, identify the source edit produced by a UI action, and describe exactly which samples support an observation. Those are more useful checks of understanding than memorizing every class name.
