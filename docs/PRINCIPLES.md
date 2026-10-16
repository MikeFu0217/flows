# Flows principles

Flows uses code to build, play and understand an instrument.

## The author keeps the direction

An instrument should help its author express a particular musical intention. AI can explain a signal path, implement an idea or suggest a change. The author still decides what sounds right, what to keep and when to stop.

## Why code belongs here

Code makes signal connections explicit. A creator can name an intermediate signal, reuse it, save a version and share the complete construction.

Observation is the central design choice. A `probe` refers to an existing signal. The creator can inspect its waveform or spectrum, audition supported audio signals, and write expressions that compare processing stages. `power(before) - power(after)` asks a different question from `power(before - after)`. The language keeps that choice visible.

The working hypothesis is that this short path between an audible change and an inspectable explanation can help people understand their own instruments. The current implementation demonstrates the mechanism. It does not establish a measured learning benefit or replace listening with a score.

## Product rules

- Source describes the instrument. Knobs, dropdowns and accepted AI proposals edit that source and use the same undo history.
- Keep ordinary code flexible. A one-line filter should not require a new module wrapper. Instruments do not implicitly share runtime instances.
- Named signals, inline capsules, the Inspector and Signal Flow provide ways to find and observe processing. Adding an observation must not restart its sound.
- The workspace gives files, code, observation and AI distinct places. Layout, font and appearance remain adjustable.
- Compilation, analysis, disk access and network work stay outside the audio callback. Optimizations must preserve event timing, independent state and observable signal values.
- Use plain English in the interface, documentation and code comments. Keep names consistent with the implementation: event, signal, voice, probe and source.

The implementation, reference documentation and recorded validation define the current product. Learning guides and presentation materials explain that implementation without claiming planned capabilities already exist.
