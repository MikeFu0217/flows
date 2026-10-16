# Understand the Flows codebase

This guide follows the implementation shipped with the ADCxGather materials. It assumes you can read a small function, but does not assume knowledge of audio programming, compilers, JUCE or React.

Keep the source open beside these chapters. Links lead to the actual files. Symbol names are the durable navigation points; line numbers are reading aids and can move after an edit. The examples are executable Flows programs, not Python programs.

| Read | What you should be able to explain afterwards |
| --- | --- |
| [00. The system in one pass](00-system-map.md) | Follow one note and one source edit through the application |
| [01. Language and signals](01-language-and-signals.md) | Read an instrument, distinguish event policy from signal processing, understand units and state |
| [02. Compiler](02-compiler.md) | Follow text into typed nodes, bindings, completion conditions and observations |
| [03. Audio engine](03-audio-engine.md) | Explain the sample loop and the oscillator, envelope, filter, LFO and distortion algorithms |
| [04. Realtime session and host](04-realtime-and-host.md) | Explain queues, note identity, program changes and the host boundary |
| [05. Probe and analysis](05-probe-and-analysis.md) | Follow captured samples into waveform, FFT, mel, power and differences |
| [06. Editor, workspace and AI](06-editor-workspace-and-ai.md) | Follow a gesture into source, undo, compilation, persistence and an AI proposal |
| [07. Verification and changes](07-verification-and-changes.md) | Find the appropriate test and know where a new capability would belong |

For individual language features, use the [example catalogue](../../examples/ADCxGather/README.md). The [language reference](../LANGUAGE.md) is the place to check defaults and accepted values.

The chapters explain current behavior. They do not claim general Python support, JIT compilation, arbitrary file imports, a global effect rack or complete Serum parity. Benchmark results apply only to their recorded workload and build.
