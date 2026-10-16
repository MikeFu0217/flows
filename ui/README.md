# Editor and native bridge

The React/CodeMirror UI edits `.flows` source. Native compiler metadata supplies
signal spans, graph nodes, parameters, choices and diagnostics. DSP remains in
the C++ engine; the browser renders captured measurements rather than a second
simulation of the instrument.

```sh
npm ci
npm run build
npm run dev
```

The Vite development server can preview layout. Audio, files, credentials and
AI tools need the native host. Use the production macOS app to verify them.

## Interaction

- Each editor group owns tabs, selection and scroll; focus chooses the playing
  instrument and Signal Flow. AI is workspace-wide with optional references.
- Cmd+K switches Edit/Perform. Perform locks source typing while keeping
  parameter drags and option choices available. Computer keys play notes.
- Completion resolves incomplete source, key aliases, built-in/class ports,
  enums, imports, units and sample paths. Types/defaults accompany suggestions.
- Numeric controls share source ownership, musical response, Shift adjustment,
  exact entry and one undo per gesture. Stale source spans cannot edit a draft.
- Invalid source keeps diagnostics and mutes the instrument. Valid unsaved
  source drives audio; Save persists it.
- Probes, capsules and Signal Flow use measured capture. Audio defaults to a
  spectrum; expanded views stack waveform above spectrum. Sustain/release stay
  live, quiet previews clear, and Hold explicitly freezes observation.
- Sequencer lanes select their own instrument/sample, level and mute. Running
  transport enables continuous beat-aligned observation even when lanes are mute.

## Bridge contract

`flowsRequest` receives `{id, method, params}` and returns
`{id, ok, result, error}`. `flowsState` publishes document/runtime state;
`flowsTelemetry` carries observations. The native browser accepts only bundled
resources and does not expose privileged integration to arbitrary websites.

`DocumentSession` serializes source edits and activation. Mutations carry
document ID and expected revision; a conflict preserves the user's draft.
Compiler UTF-8 source spans are converted to UTF-16 positions against that
same revision. Numeric gesture IDs/commit/cancel phases preserve undo ownership.

See [the editor implementation guide](../docs/learn/06-editor-workspace-and-ai.md)
and [the architecture](../docs/ARCHITECTURE.md) for implementation paths.
