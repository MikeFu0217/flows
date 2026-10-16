#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flows {

using ValueId = std::uint32_t;

struct SourceSpan {
    std::size_t offset = 0;
    std::size_t length = 0;
    std::size_t line = 1;
    std::size_t column = 1;
};

struct Diagnostic {
    SourceSpan span;
    std::string message;
};

enum class Unit { scalar, pitch, hertz, decibels, seconds, boolean };
enum class Rate { constant, event_held, sample_varying };
enum class Op {
    parameter, key_pitch, key_gate, key_velocity,
    pitch_to_hz, db_to_linear, sine,
    add, subtract, multiply, divide,
    key_trigger, saw, adsr, filter_lowpass, filter_highpass,
    less, less_equal, greater, greater_equal, equal, not_equal, select,
    unison_sine, unison_saw, lfo, distortion, stereo, left, right, mid, side, sampler,
    phasor, math_sin, math_cos
};

struct Node {
    Op op = Op::parameter;
    std::vector<ValueId> inputs;
    double initial_value = 0.0;
    Unit unit = Unit::scalar;
    Rate rate = Rate::constant;
    SourceSpan span;
    std::uint32_t state_index = 0;
    std::uint32_t flags = 0;
    // Mono control/audio, or stereo audio. Arithmetic broadcasts mono operands.
    std::uint32_t channels = 1;
    // Sampler nodes address immutable, off-thread loaded resources.
    std::uint32_t resource_index = 0;
};
// Extended oscillator inputs: frequency, gain, detune (cents), spread, pan,
// phase, random. Flags contain the constant unison count (1..8).
// LFO inputs: rate Hz or whole-note division, trigger, phase, delay, fade.
// Flags: shape bits 0..2, mode bits 3..4, bipolar bit 5, tempo sync bit 6.
// Distortion inputs: input, drive linear, wet mix, output linear.
// Flags: softclip=0, hardclip=1, fold=2.
// Sampler inputs: trigger, pitch (MIDI), root (MIDI), rate multiplier,
// normalized start, normalized end, linear gain, gate. Flags: loop bit 0,
// reverse bit 1. Output is always stereo; mono files duplicate their channel.

inline constexpr std::uint32_t retrigger_from_zero = 1;
inline constexpr ValueId invalid_value = static_cast<ValueId>(-1);

enum class StateKind { oscillator, envelope, filter, lfo, distortion, sampler };
struct StateSpec {
    std::string name;
    SourceSpan span;
    StateKind kind = StateKind::oscillator;
};
using OscillatorState = StateSpec;

enum class VoiceMode { poly, mono };
enum class Retrigger { each_press, gate_rise };
struct VoicePolicy {
    VoiceMode mode = VoiceMode::poly;
    Retrigger retrigger = Retrigger::each_press;
    double glide_seconds = 0.0;
};

enum class CompletionOp { gate_closed, envelope_idle, filter_settled, all, any, distortion_settled, sample_done };
struct CompletionExpr {
    CompletionOp op = CompletionOp::gate_closed;
    std::uint32_t state_index = 0;
    std::vector<std::uint32_t> inputs;
};

struct ParameterDescriptor {
    ValueId value = invalid_value;
    std::string binding_id;
    double min = -1000000.0;
    double max = 1000000.0;
    double step = 0.01;
    bool logarithmic = false;
    double smoothing_seconds = 0.0;
    Unit unit = Unit::scalar;
    SourceSpan span;
    bool live = true;
    bool has_range = false;
    double default_value = 0.0;
    // True when the owning class declares what this port falls back to. Only
    // then does a default exist that a UI gesture can meaningfully restore.
    bool has_default = false;
    std::string name;
    bool discrete = false;
    // Presentation response is independent of DSP smoothing and saved DAW mapping.
    std::string control_scale = "linear";
    double control_offset = 0.0;
};

struct ChoiceOption {
    std::string value;
    bool enabled = true;
    std::string reason = {};
};
struct ChoiceDescriptor {
    std::string binding_id, name, selected, owner;
    // Explicit values replace this span. Implicit defaults insert at a zero-
    // length span using prefix + option.value + suffix.
    SourceSpan span;
    bool implicit = false;
    std::string prefix, suffix;
    std::vector<ChoiceOption> options;
};

enum class AnalysisOp { probe, constant, add, subtract, multiply, divide, power, spectrum, mel_spectrum };
enum class AnalysisShape { waveform, scalar, spectrum, mel_spectrum };
struct AnalysisNode {
    AnalysisOp op = AnalysisOp::probe;
    std::vector<std::uint32_t> inputs;
    ValueId signal = invalid_value;
    double initial_value = 0.0;
    AnalysisShape shape = AnalysisShape::waveform;
    Unit unit = Unit::scalar;
    SourceSpan span;
};
struct NamedAnalysis {
    std::string name;
    std::uint32_t value = 0;
    SourceSpan span;
};

struct NamedValue {
    std::string name;
    ValueId value = 0;
    SourceSpan span;
};

struct SampleResource {
    // Portable path relative to the host's asset root. The compiler
    // validates its spelling; resource loading and containment checks are host work.
    std::string path;
    SourceSpan span;
};

// Immutable after compilation. Runtime phase and parameter values live in Engine.
struct Program {
    std::string source_path;
    std::vector<Node> nodes;
    std::vector<StateSpec> states;
    std::vector<NamedValue> named_values;
    ValueId output = 0;
    VoicePolicy voice_policy;
    std::vector<CompletionExpr> completion;
    std::optional<std::uint32_t> completion_root;
    std::vector<ParameterDescriptor> parameters;
    std::vector<ChoiceDescriptor> choices;
    std::vector<AnalysisNode> analysis;
    std::vector<NamedAnalysis> named_analysis;
    std::vector<SampleResource> samples;
};

struct CompileResult {
    std::optional<Program> program;
    std::vector<Diagnostic> diagnostics;
};

CompileResult compile(std::string_view source, std::string source_path);
std::string program_json(const Program& program);
// Canonical DSP/state/binding identity, excluding source locations, analysis,
// and authored live values. This is a comparison key, not a security hash.
std::string dsp_structure_key(const Program& program);
std::string_view op_name(Op op);
std::string_view unit_name(Unit unit);
std::string_view rate_name(Rate rate);

} // namespace flows
