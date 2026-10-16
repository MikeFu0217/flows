#include "engine/engine.hpp"
#include "engine/boundary_smoother.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace flows {
namespace {
constexpr std::uint32_t voice_limit = 32;
constexpr std::size_t event_limit = 256, held_limit = 1024;
constexpr std::uint32_t no_slot = std::numeric_limits<std::uint32_t>::max();
constexpr double pi = 3.1415926535897932384626433832795;

std::size_t arity(Op op) {
    switch (op) {
    case Op::parameter: case Op::key_pitch: case Op::key_gate:
    case Op::key_velocity: case Op::key_trigger: return 0;
    case Op::pitch_to_hz: case Op::db_to_linear: case Op::left: case Op::right: case Op::mid: case Op::side:
    case Op::phasor: case Op::math_sin: case Op::math_cos: return 1;
    case Op::sine: case Op::saw: case Op::add: case Op::subtract:
    case Op::multiply: case Op::divide: case Op::less: case Op::less_equal:
    case Op::greater: case Op::greater_equal: case Op::equal: case Op::not_equal: case Op::stereo: return 2;
    case Op::filter_lowpass: case Op::filter_highpass: case Op::select: return 3;
    case Op::distortion: return 4;
    case Op::lfo: return 5;
    case Op::adsr: return 6;
    case Op::unison_sine: case Op::unison_saw: return 7;
    case Op::sampler: return 8;
    }
    throw std::invalid_argument("unknown DSP operation");
}

bool stateful(Op op) noexcept {
    return op == Op::sine || op == Op::saw || op == Op::adsr ||
           op == Op::filter_lowpass || op == Op::filter_highpass ||
           op == Op::unison_sine || op == Op::unison_saw || op == Op::lfo || op == Op::distortion || op == Op::sampler || op == Op::phasor;
}

std::uint64_t samples_for(double seconds, double rate) noexcept {
    if (!std::isfinite(seconds)) return 0;
    return static_cast<std::uint64_t>(std::floor(std::clamp(seconds, 0.0, 600.0) * rate + 0.5));
}

struct Envelope {
    enum class Stage { idle, attack, decay, sustain, release };
    Stage stage = Stage::idle;
    double level = 0.0, start = 0.0, target = 0.0;
    std::uint64_t elapsed = 0, length = 0;
    bool previous_gate = false;

    void enter(Stage next, double destination, std::uint64_t duration) noexcept {
        stage = next; start = level; target = destination; elapsed = 0; length = duration;
    }

    void next(double decay, double sustain, double rate) noexcept {
        level = target;
        if (stage == Stage::attack) enter(Stage::decay, sustain, samples_for(decay, rate));
        else if (stage == Stage::decay) stage = Stage::sustain;
        else if (stage == Stage::release) { stage = Stage::idle; level = 0.0; }
    }

    float tick(bool gate, bool trigger, bool from_zero, double attack, double decay,
               double sustain, double release, double rate, bool& idle) noexcept {
        sustain = std::clamp(sustain, 0.0, 1.0);
        if ((gate && !previous_gate) || trigger) {
            if (from_zero) level = 0.0;
            enter(Stage::attack, 1.0, samples_for(attack, rate));
        }
        // A trigger and a release at the same sample cannot leave a hanging envelope.
        if (!gate && stage != Stage::idle && stage != Stage::release)
            enter(Stage::release, 0.0, samples_for(release, rate));
        previous_gate = gate;
        for (int i = 0; i < 3 && stage != Stage::idle && stage != Stage::sustain && elapsed >= length; ++i)
            next(decay, sustain, rate);
        if (stage == Stage::decay && length != 0) {
            target = sustain;
            level = start + (target - start) * (double(elapsed) / double(length));
        }
        if (stage == Stage::sustain) level = sustain;
        idle = stage == Stage::idle;
        const float result = static_cast<float>(level);
        if (stage != Stage::idle && stage != Stage::sustain) {
            ++elapsed;
            if (elapsed >= length) {
                level = target;
                // Release completion is known before the next event is admitted.
                // Attack/Decay read the next stage's parameters at its actual start sample.
                if (stage == Stage::release) stage = Stage::idle;
            }
            else level = start + (target - start) * (double(elapsed) / double(length));
        }
        return result;
    }
};

struct FilterState {
    double ic1 = 0.0, ic2 = 0.0;
    double cutoff = -1.0, resonance = -1.0, k = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
};
struct DistortionState { double previous = 0.0, wet = 0.0; };
struct SamplerState {
    double position = 0.0;
    std::size_t start = 0, end = 0;
    bool initialized = false, done = false;
    double previous_pitch = -1.0, previous_root = -1.0, previous_rate = -1.0;
    double increment = 1.0, cutoff_fraction = 0.0;
    std::size_t cutoff_row = 0;
};
struct DspState {
    double phase = 0.0;
    Envelope envelope;
    SamplerState sampler;
    std::array<FilterState, 2> filters{};
    std::array<DistortionState, 2> distortion{};
    std::array<double, 8> phases{}, detune_ratio{}, pan_left{}, pan_right{};
    double previous_detune = -1.0, previous_spread = -1.0, previous_pan = -2.0;
    std::uint64_t oscillator_note = 0, lfo_elapsed = 0, lfo_seed = 0;
    std::int64_t lfo_cycle = 0;
    bool oscillator_initialized = false, lfo_initialized = false, lfo_done = false;
};

// 32-tap Blackman-windowed sinc. Phase and ratio tables are immutable and shared;
// construction is explicitly forced during Engine preparation, never rendering.
constexpr std::size_t sinc_taps = 32, sinc_phases = 128, sinc_cutoffs = 33;
using SincRow = std::array<float, sinc_taps>;
using SincTable = std::array<std::array<SincRow, sinc_phases + 1>, sinc_cutoffs>;
const SincTable& sinc_table() {
    static const auto prepared = [] {
        auto table = std::make_unique<SincTable>();
        for (std::size_t c = 0; c < sinc_cutoffs; ++c) {
            const auto cutoff = std::exp2(-double(c) / 4.0);
            for (std::size_t phase = 0; phase <= sinc_phases; ++phase) {
                double sum = 0;
                auto& row = (*table)[c][phase];
                for (std::size_t tap = 0; tap < sinc_taps; ++tap) {
                    const auto distance = double(tap) - 15.0 - double(phase) / sinc_phases;
                    const auto x = distance * cutoff;
                    const auto window = 0.42 + 0.5 * std::cos(pi * distance / 16.0) + 0.08 * std::cos(2.0 * pi * distance / 16.0);
                    const auto weight = cutoff * (std::abs(x) < 1e-12 ? 1.0 : std::sin(pi * x) / (pi * x)) * window;
                    row[tap] = static_cast<float>(weight); sum += weight;
                }
                for (auto& weight : row) weight = static_cast<float>(double(weight) / sum);
            }
        }
        return table;
    }();
    return *prepared;
}

double random_unit(std::uint64_t seed) noexcept {
    seed += 0x9e3779b97f4a7c15ULL;
    seed = (seed ^ (seed >> 30)) * 0xbf58476d1ce4e5b9ULL;
    seed = (seed ^ (seed >> 27)) * 0x94d049bb133111ebULL;
    seed ^= seed >> 31;
    return static_cast<double>(seed >> 11) * 0x1.0p-53;
}

double wrap_phase(double phase) noexcept { return phase - std::floor(phase); }
std::int64_t cycle_index(double phase) noexcept {
    // Hash periods remain deterministic without converting an out-of-range host PPQ.
    return static_cast<std::int64_t>(std::fmod(std::floor(phase), 0x1.0p52));
}

double waveshape(double value, std::uint32_t mode) noexcept {
    if (mode == 0) return std::tanh(value);
    if (mode == 1) return std::clamp(value, -1.0, 1.0);
    const auto wrapped = value + 1.0 - 4.0 * std::floor((value + 1.0) / 4.0);
    return 1.0 - std::abs(wrapped - 2.0);
}

double antiderivative(double value, std::uint32_t mode) noexcept {
    if (mode == 0) {
        const auto x = std::abs(value);
        return x + std::log1p(std::exp(-2.0 * x)) - 0.6931471805599453;
    }
    if (mode == 1) {
        const auto x = std::abs(value);
        return x <= 1.0 ? 0.5 * x * x : x - 0.5;
    }
    const auto x = value + 1.0 - 4.0 * std::floor((value + 1.0) / 4.0);
    return (x <= 2.0 ? 0.5 * x * x - x : 3.0 * x - 0.5 * x * x - 4.0) + 0.5;
}

float distortion_tick(DistortionState& state, double input, double drive, double mix,
                      double output, std::uint32_t mode) noexcept {
    const auto driven = input * drive;
    const auto difference = driven - state.previous;
    // First-order antiderivative antialiasing integrates the nonlinear curve over
    // each sample interval. Its midpoint fallback avoids cancellation at DC.
    // Parker et al., DAFx-2016: https://github.com/julian-parker/DAFX-AntiAliasing
    const auto wet = std::abs(difference) > 1e-6 * (1.0 + std::max(std::abs(driven), std::abs(state.previous)))
        ? (antiderivative(driven, mode) - antiderivative(state.previous, mode)) / difference
        : waveshape(0.5 * (driven + state.previous), mode);
    state.previous = driven;
    mix = std::clamp(mix, 0.0, 1.0);
    state.wet = mix == 0.0 ? 0.0 : wet;
    return static_cast<float>((mix == 0.0 ? input : input + mix * (wet - input)) * output);
}

double lfo_wave(double phase, std::int64_t cycle, std::uint32_t shape, std::uint64_t seed) noexcept {
    switch (shape) {
    case 0: return std::sin(2.0 * pi * phase);
    case 1: return 1.0 - 4.0 * std::abs(phase - 0.5);
    case 2: return 2.0 * phase - 1.0;
    case 3: return phase < 0.5 ? 1.0 : -1.0;
    default: return 2.0 * random_unit(seed ^ static_cast<std::uint64_t>(cycle)) - 1.0;
    }
}

double blep(double phase, double delta) noexcept {
    // Two-sample polynomial BLEP residual; reduces aliasing, not ideally bandlimited.
    // https://mac.kaist.ac.kr/pubs/ValimakiPeknenNam-jasa2012.pdf
    if (delta <= 0.0) return 0.0;
    if (phase < delta) { const auto x = phase / delta; return 2.0 * x - x * x - 1.0; }
    if (phase > 1.0 - delta) { const auto x = (phase - 1.0) / delta; return x * x + 2.0 * x + 1.0; }
    return 0.0;
}

float filter_tick(FilterState& state, float input, double cutoff, double resonance,
                  double rate, bool highpass) noexcept {
    // Trapezoidal state-variable filter (Andrew Simper / Cytomic derivation).
    // https://cytomic.com/files/dsp/SvfLinearTrapOptimised2.pdf
    cutoff = std::clamp(cutoff, 5.0, rate * 0.45);
    resonance = std::clamp(resonance, 0.0, 1.0);
    if (cutoff != state.cutoff || resonance != state.resonance) {
        state.cutoff = cutoff; state.resonance = resonance;
        const auto g = std::tan(pi * cutoff / rate);
        // Normalized resonance changes damping: Butterworth at 0, finite Q at 1.
        state.k = 1.4142135623730951 * (1.0 - 0.95 * resonance);
        state.a1 = 1.0 / (1.0 + g * (g + state.k));
        state.a2 = g * state.a1; state.a3 = g * state.a2;
    }
    const auto v3 = double(input) - state.ic2;
    const auto v1 = state.a1 * state.ic1 + state.a2 * v3;
    const auto v2 = state.ic2 + state.a2 * state.ic1 + state.a3 * v3;
    state.ic1 = 2.0 * v1 - state.ic1;
    state.ic2 = 2.0 * v2 - state.ic2;
    if (std::abs(state.ic1) < 1e-25) state.ic1 = 0.0;
    if (std::abs(state.ic2) < 1e-25) state.ic2 = 0.0;
    return static_cast<float>(highpass ? double(input) - state.k * v1 - v2 : v2);
}
} // namespace

struct Engine::Impl {
    struct Voice {
        bool allocated = false, sounding = false, gate = false, trigger = false;
        double pitch = 69.0, velocity = 0.0, glide_start = 69.0, glide_target = 69.0;
        std::uint64_t glide_elapsed = 0, glide_length = 0, note_id = 0, generation = 0, onset = 0;
        std::uint32_t attack_elapsed = 0, gain_remaining = 0;
        float output_gain = 1, gain_target = 1;
    };
    struct HeldNote {
        bool held = false;
        std::uint64_t note_id = 0, order = 0;
        std::uint32_t slot = no_slot;
        double pitch = 69.0, velocity = 0.0;
    };
    struct Parameter {
        double current = 0.0, start = 0.0, target = 0.0;
        std::uint64_t elapsed = 0, length = 0;
        std::uint32_t descriptor = no_slot;
        bool explicit_ramp = false, ramp_logarithmic = false;
    };

    Program program;
    EngineConfig config;
    SampleBank samples;
    const SincTable* sample_kernel = nullptr;
    EngineStats stats;
    std::array<Voice, voice_limit> voices{};
    std::array<BoundarySmoother, voice_limit> boundaries{};
    std::uint32_t boundary_length = 0;
    std::uint32_t attack_length = 0;
    std::array<HeldNote, held_limit> held{};
    std::uint64_t press_order = 0;
    std::vector<DspState> states;
    std::vector<Parameter> parameters;
    std::vector<float> scratch, controls;
    std::vector<std::size_t> offsets;
    Transport transport;
    std::uint64_t transport_anchor = 0;
    std::vector<unsigned char> named_taps, state_idle, completion_samples, completion_now;
    bool mono = false;
    std::array<OutputGainGroup,8> output_groups{};
    std::size_t output_group_count = 0;
    float default_output_gain = 1;
    float gain_for(std::uint64_t note) const noexcept {
        for(std::size_t i=0;i<output_group_count;++i) if((note&output_groups[i].mask)==output_groups[i].value)return output_groups[i].gain;
        return default_output_gain;
    }
    void update_output_gain(Voice& voice) noexcept {
        const auto target=gain_for(voice.note_id);
        if(target!=voice.gain_target){voice.gain_target=target;voice.gain_remaining=std::max(1U,static_cast<unsigned>(config.sample_rate*.005));}
    }
    float output_gain(Voice& voice) noexcept {
        if(voice.gain_remaining){voice.output_gain+=(voice.gain_target-voice.output_gain)/voice.gain_remaining;--voice.gain_remaining;}
        return voice.output_gain;
    }


    Impl(const Program& source, EngineConfig prepare, SampleBank bank) : program(source), config(prepare), samples(std::move(bank)) {
        if (!std::isfinite(config.sample_rate) || config.sample_rate < 100.0 || config.sample_rate > 768000.0 ||
            config.max_block_size == 0 || config.max_block_size > 65536 ||
            config.max_voices == 0 || config.max_voices > voice_limit ||
            !std::isfinite(config.boundary_seconds) || config.boundary_seconds < 0 || config.boundary_seconds > 0.020)
            throw std::invalid_argument("invalid engine preparation configuration");
        boundary_length = static_cast<std::uint32_t>(samples_for(config.boundary_seconds, config.sample_rate));
        attack_length = boundary_length ? std::max(1U, boundary_length / 3) : 0;
        if (program.nodes.empty() || program.output >= program.nodes.size() || program.nodes.size() > 16384 ||
            program.states.size() > 4096 || program.completion.size() > 32768)
            throw std::invalid_argument("invalid or oversized DSP program");
        if (samples.size() != program.samples.size() || samples.size() > 64)
            throw std::invalid_argument("sample bank does not match program resources");
        std::vector<const SampleData*> validated;
        std::size_t sample_bytes = 0;
        for (const auto& item : samples) {
            if (!item || !std::isfinite(item->sample_rate) || item->sample_rate < 100 || item->sample_rate > 768000 ||
                item->left.empty() || (!item->right.empty() && item->right.size() != item->left.size()))
                throw std::invalid_argument("invalid decoded sample resource");
            if (std::find(validated.begin(), validated.end(), item.get()) != validated.end()) continue;
            const auto count = item->left.size() + item->right.size();
            if (count > (256 * 1024 * 1024 - sample_bytes) / sizeof(float) ||
                double(item->left.size()) / item->sample_rate > 600)
                throw std::invalid_argument("sample bank exceeds preparation budget");
            sample_bytes += count * sizeof(float);
            const auto finite = [](float value) { return std::isfinite(value); };
            if (!std::all_of(item->left.begin(), item->left.end(), finite) || !std::all_of(item->right.begin(), item->right.end(), finite))
                throw std::invalid_argument("decoded sample contains non-finite audio");
            validated.push_back(item.get());
        }
        if (!samples.empty()) sample_kernel = &sinc_table();
        offsets.resize(program.nodes.size());
        std::size_t scratch_count = 0;
        for (std::size_t id = 0; id < program.nodes.size(); ++id) {
            if (program.nodes[id].channels < 1 || program.nodes[id].channels > 2)
                throw std::invalid_argument("DSP signals support one or two channels");
            offsets[id] = scratch_count;
            scratch_count += program.nodes[id].channels * config.max_block_size;
        }
        const auto auxiliary_count = (program.states.size() + program.completion.size()) * config.max_block_size;
        const auto state_bytes = program.states.size() * (program.voice_policy.mode == VoiceMode::mono ? 1U : config.max_voices) * sizeof(DspState);
        if (scratch_count * sizeof(float) + auxiliary_count + state_bytes > 256 * 1024 * 1024)
            throw std::invalid_argument("DSP scratch/state exceeds 256 MiB preparation budget");
        if (!std::isfinite(program.voice_policy.glide_seconds) || program.voice_policy.glide_seconds < 0 ||
            program.voice_policy.glide_seconds > 600)
            throw std::invalid_argument("invalid glide duration");
        mono = program.voice_policy.mode == VoiceMode::mono;
        std::vector<bool> invoked(program.states.size(), false);
        for (std::size_t id = 0; id < program.nodes.size(); ++id) {
            const auto& node = program.nodes[id];
            if (node.inputs.size() != arity(node.op)) throw std::invalid_argument("incorrect DSP input count");
            for (const auto input : node.inputs)
                if (input >= id) throw std::invalid_argument("DSP inputs must precede their consumer");
            const bool unison = node.op == Op::unison_sine || node.op == Op::unison_saw;
            if (unison && (node.flags < 1 || node.flags > 8 || node.channels != 2))
                throw std::invalid_argument("unison requires 1..8 oscillators and stereo output");
            if (node.op == Op::lfo && ((node.flags & 7) > 4 || (node.flags & ~127U) || node.channels != 1))
                throw std::invalid_argument("invalid LFO configuration");
            if (node.op == Op::distortion && node.flags > 2)
                throw std::invalid_argument("invalid distortion mode");
            if (node.op == Op::sampler && (node.flags > 3 || node.channels != 2 || node.resource_index >= samples.size()))
                throw std::invalid_argument("invalid sampler resource or configuration");
            const bool projection = node.op == Op::left || node.op == Op::right || node.op == Op::mid || node.op == Op::side;
            if (projection && node.channels != 1)
                throw std::invalid_argument("channel projection requires mono output");
            if (node.op == Op::stereo && (node.channels != 2 || program.nodes[node.inputs[0]].channels != 1 || program.nodes[node.inputs[1]].channels != 1))
                throw std::invalid_argument("stereo requires two mono inputs");
            if (!projection && node.op != Op::stereo)
                for (auto input : node.inputs) if (program.nodes[input].channels > node.channels)
                    throw std::invalid_argument("stereo input cannot silently become mono");
            if ((node.op == Op::parameter || node.op == Op::sine || node.op == Op::saw || node.op == Op::adsr || node.op == Op::phasor) && node.channels != 1)
                throw std::invalid_argument("parameters and legacy stateful primitives must be mono");
            if (unison || node.op == Op::sampler || node.op == Op::lfo || node.op == Op::adsr || node.op == Op::sine || node.op == Op::saw || node.op == Op::phasor)
                for (auto input : node.inputs) if (program.nodes[input].channels != 1)
                    throw std::invalid_argument("oscillator and control parameters must be mono");
            if (node.op == Op::parameter && (!std::isfinite(node.initial_value) ||
                std::abs(node.initial_value) > std::numeric_limits<float>::max()))
                throw std::invalid_argument("parameter is not a finite signal");
            if (stateful(node.op)) {
                if (node.state_index >= program.states.size() || invoked[node.state_index])
                    throw std::invalid_argument("state must have exactly one producer");
                invoked[node.state_index] = true;
                const auto expected = node.op == Op::adsr ? StateKind::envelope :
                    node.op == Op::lfo ? StateKind::lfo : node.op == Op::distortion ? StateKind::distortion :
                    node.op == Op::sampler ? StateKind::sampler :
                    (node.op == Op::sine || node.op == Op::saw || node.op == Op::phasor || unison ? StateKind::oscillator : StateKind::filter);
                if (program.states[node.state_index].kind != expected)
                    throw std::invalid_argument("DSP state kind does not match its producer");
            }
        }
        if (program.completion_root && *program.completion_root >= program.completion.size())
            throw std::invalid_argument("invalid completion root");
        for (std::size_t i = 0; i < program.completion.size(); ++i) {
            const auto& expression = program.completion[i];
            for (const auto child : expression.inputs)
                if (child >= i) throw std::invalid_argument("completion expressions must be topological");
            if (expression.op == CompletionOp::envelope_idle || expression.op == CompletionOp::filter_settled || expression.op == CompletionOp::distortion_settled || expression.op == CompletionOp::sample_done) {
                const auto expected = expression.op == CompletionOp::envelope_idle ? StateKind::envelope :
                    expression.op == CompletionOp::distortion_settled ? StateKind::distortion :
                    expression.op == CompletionOp::sample_done ? StateKind::sampler : StateKind::filter;
                if (expression.state_index >= program.states.size() || !invoked[expression.state_index] ||
                    program.states[expression.state_index].kind != expected)
                    throw std::invalid_argument("invalid completion state");
            }
        }
        states.resize(program.states.size() * (mono ? 1U : config.max_voices));
        parameters.resize(program.nodes.size());
        for (std::size_t i = 0; i < parameters.size(); ++i)
            parameters[i].current = parameters[i].start = parameters[i].target = program.nodes[i].initial_value;
        for (std::size_t i = 0; i < program.parameters.size(); ++i) {
            const auto& descriptor = program.parameters[i];
            if (!descriptor.live) continue;
            if (descriptor.value >= program.nodes.size() || program.nodes[descriptor.value].op != Op::parameter ||
                parameters[descriptor.value].descriptor != no_slot || !std::isfinite(descriptor.smoothing_seconds) ||
                descriptor.smoothing_seconds < 0 || descriptor.smoothing_seconds > 600 ||
                (descriptor.has_range && (!std::isfinite(descriptor.min) || !std::isfinite(descriptor.max) || descriptor.min > descriptor.max)))
                throw std::invalid_argument("invalid live parameter descriptor");
            if (descriptor.has_range && (parameters[descriptor.value].current < descriptor.min ||
                                         parameters[descriptor.value].current > descriptor.max))
                throw std::invalid_argument("initial parameter lies outside its declared range");
            parameters[descriptor.value].descriptor = static_cast<std::uint32_t>(i);
        }
        scratch.resize(scratch_count); controls.resize(4 * config.max_block_size);
        state_idle.resize(program.states.size() * config.max_block_size);
        completion_samples.resize(program.completion.size() * config.max_block_size);
        completion_now.resize(program.completion.size()); named_taps.resize(program.nodes.size());
        for (const auto& named : program.named_values) {
            if (named.value >= program.nodes.size()) throw std::invalid_argument("named signal does not exist");
            named_taps[named.value] = 1;
        }
        for (const auto& analysis : program.analysis) if (analysis.op == AnalysisOp::probe) {
            if (analysis.signal >= program.nodes.size()) throw std::invalid_argument("probe signal does not exist");
            named_taps[analysis.signal] = 1;
        }
        if (mono) { voices[0].allocated = true; voices[0].generation = 1; stats.voice_instance_count = 1; }
    }

    float* data(ValueId value, std::uint32_t channel = 0) noexcept {
        return scratch.data() + offsets[value] + std::min<std::uint32_t>(channel, program.nodes[value].channels - 1) * config.max_block_size;
    }
    double ppq_at(std::uint64_t sample) const noexcept {
        return transport.ppq + (transport.playing ? double(sample - transport_anchor) * transport.bpm / (60.0 * config.sample_rate) : 0.0);
    }
    void set_transport(Transport next) noexcept {
        if (!std::isfinite(next.bpm) || next.bpm <= 0.0 || next.bpm > 1000.0) next.bpm = 120.0;
        if (!std::isfinite(next.ppq)) next.positioned = false;
        if (!next.positioned && next.bpm == transport.bpm && next.playing == transport.playing) return;
        next.ppq = next.positioned ? next.ppq : ppq_at(stats.processed_samples);
        transport_anchor = stats.processed_samples;
        transport = next;
    }
    DspState& state(std::uint32_t slot, std::uint32_t index) noexcept { return states[slot * program.states.size() + index]; }

    void panic() noexcept {
        held.fill({});
        boundaries.fill({});
        for (auto& voice : voices) { const auto generation = voice.generation; voice = {}; voice.generation = generation; }
        std::fill(states.begin(), states.end(), DspState{});
        stats.active_voices = 0; stats.voice_instance_count = mono ? 1 : 0;
        if (mono) { voices[0].allocated = true; ++voices[0].generation; }
    }

    ProcessError validate(std::span<const Event> events, std::span<const ParameterUpdate> updates,
                          std::size_t frames) const noexcept {
        if (frames > config.max_block_size || frames > std::numeric_limits<std::uint64_t>::max() - stats.processed_samples)
            return ProcessError::block_capacity;
        if (events.size() > event_limit) return ProcessError::event_capacity;
        if (updates.size() > event_limit) return ProcessError::parameter_capacity;
        const auto end = stats.processed_samples + frames;
        auto previous = stats.processed_samples;
        for (const auto& update : updates) {
            if (update.sample < previous) return ProcessError::event_order;
            if (update.sample >= end) return ProcessError::event_range;
            previous = update.sample;
            if (update.parameter >= parameters.size() || parameters[update.parameter].descriptor == no_slot ||
                !std::isfinite(update.value) || std::abs(update.value) > std::numeric_limits<float>::max())
                return ProcessError::invalid_parameter;
            const auto& descriptor = program.parameters[parameters[update.parameter].descriptor];
            if (descriptor.has_range && (update.value < descriptor.min || update.value > descriptor.max))
                return ProcessError::invalid_parameter;
            if (update.ramp_start && (!update.ramp_samples || !std::isfinite(*update.ramp_start) ||
                std::abs(*update.ramp_start) > std::numeric_limits<float>::max() ||
                (descriptor.has_range && (*update.ramp_start < descriptor.min || *update.ramp_start > descriptor.max))))
                return ProcessError::invalid_parameter;
        }
        if (events.empty()) return ProcessError::none;
        auto shadow = held;
        previous = stats.processed_samples;
        for (const auto& event : events) {
            if (event.sample < previous) return ProcessError::event_order;
            if (event.sample >= end) return ProcessError::event_range;
            previous = event.sample;
            auto found = std::find_if(shadow.begin(), shadow.end(), [&](const HeldNote& note) { return note.held && note.note_id == event.note_id; });
            if (event.type == EventType::note_on) {
                if (!std::isfinite(event.pitch) || event.pitch < 0 || event.pitch > 127 ||
                    !std::isfinite(event.velocity) || event.velocity < 0 || event.velocity > 1)
                    return ProcessError::invalid_note;
                if (found != shadow.end()) return ProcessError::duplicate_note;
                auto free = std::find_if(shadow.begin(), shadow.end(), [](const HeldNote& note) { return !note.held; });
                if (free == shadow.end()) return ProcessError::held_capacity;
                *free = {true, event.note_id};
            } else if (event.type == EventType::note_off) {
                if (found == shadow.end()) return ProcessError::unknown_note;
                found->held = false;
            } else return ProcessError::invalid_note;
        }
        return ProcessError::none;
    }

    void parameter(const ParameterUpdate& update) noexcept {
        auto& target = parameters[update.parameter];
        if (update.ramp_start) target.current = *update.ramp_start;
        target.start = target.current; target.target = update.value; target.elapsed = 0;
        target.explicit_ramp = update.ramp_samples.has_value();
        target.ramp_logarithmic = update.ramp_logarithmic;
        target.length = update.ramp_samples ? *update.ramp_samples :
            samples_for(program.parameters[target.descriptor].smoothing_seconds, config.sample_rate);
        if (target.length == 0) target.current = target.target;
        ++stats.parameter_updates; ++stats.parameter_epoch;
    }

    void make_parameters(std::size_t count) noexcept {
        for (ValueId id = 0; id < program.nodes.size(); ++id) {
            if (program.nodes[id].op != Op::parameter) continue;
            auto& parameter = parameters[id];
            auto* destination = data(id);
            for (std::size_t i = 0; i < count; ++i) {
                destination[i] = static_cast<float>(parameter.current);
                if (parameter.elapsed < parameter.length) {
                    ++parameter.elapsed;
                    const auto fraction = double(parameter.elapsed) / double(parameter.length);
                    const bool requested_logarithmic = parameter.explicit_ramp ? parameter.ramp_logarithmic :
                        (parameter.descriptor != no_slot && program.parameters[parameter.descriptor].logarithmic);
                    const bool logarithmic = requested_logarithmic && parameter.start > 0 && parameter.target > 0;
                    parameter.current = logarithmic ? std::exp(std::log(parameter.start) +
                        (std::log(parameter.target) - std::log(parameter.start)) * fraction) :
                        parameter.start + (parameter.target - parameter.start) * fraction;
                    if (parameter.elapsed == parameter.length) parameter.current = parameter.target;
                }
            }
        }
    }

    void target_pitch(Voice& voice, double pitch, bool glide) noexcept {
        voice.glide_start = voice.pitch; voice.glide_target = pitch; voice.glide_elapsed = 0;
        voice.glide_length = glide ? samples_for(program.voice_policy.glide_seconds, config.sample_rate) : 0;
        if (voice.glide_length == 0) voice.pitch = pitch;
    }

    void apply(const Event& event, CaptureSink* captures) noexcept {
        VoiceIdentity identity{no_slot, 0, event.note_id};
        if (event.type == EventType::note_on) {
            auto ledger = std::find_if(held.begin(), held.end(), [](const HeldNote& note) { return !note.held; });
            *ledger = {true, event.note_id, ++press_order, no_slot, event.pitch, event.velocity};
            std::uint32_t slot = 0;
            if (!mono) while (slot < config.max_voices && voices[slot].allocated) ++slot;
            if (slot == config.max_voices) {
                ++stats.rejected_notes;
                if (captures) captures->event(event, identity, "rejected");
                return;
            }
            auto& voice = voices[slot];
            if (boundary_length) boundaries[slot].mark();
            const bool gate_was_high = voice.gate;
            const bool was_sounding = voice.sounding;
            if (!mono) {
                const auto generation = voice.generation + 1; voice = {}; voice.generation = generation;
                voice.allocated = true; ++stats.voice_instance_count;
                std::fill_n(states.begin() + slot * program.states.size(), program.states.size(), DspState{});
            }
            // An ungated mono graph is already audible before any key arrives.
            // Starting a note must not add an onset window to that continuous signal.
            if (mono && !was_sounding) voice.attack_elapsed = program.completion_root ? 0 : attack_length;
            target_pitch(voice, event.pitch, mono && gate_was_high);
            voice.velocity = event.velocity; voice.note_id = event.note_id;
            if(!was_sounding){voice.output_gain=voice.gain_target=gain_for(event.note_id);voice.gain_remaining=0;}
            if (!mono || !gate_was_high || program.voice_policy.retrigger == Retrigger::each_press) voice.onset = ledger->order;
            voice.trigger = voice.trigger || !mono || !gate_was_high || program.voice_policy.retrigger == Retrigger::each_press;
            voice.gate = true;
            if (!voice.sounding) { voice.sounding = true; ++stats.active_voices; }
            ledger->slot = slot; ++stats.accepted_notes;
            identity = {slot, voice.generation, event.note_id};
            if (captures) captures->event(event, identity, "accepted");
            return;
        }
        auto ledger = std::find_if(held.begin(), held.end(), [&](const HeldNote& note) { return note.held && note.note_id == event.note_id; });
        if (ledger->slot != no_slot) {
            auto& voice = voices[ledger->slot];
            if (boundary_length && (!mono || voice.note_id == event.note_id)) boundaries[ledger->slot].mark();
            identity = {ledger->slot, voice.generation, event.note_id};
            ledger->held = false;
            if (mono) {
                const HeldNote* latest = nullptr;
                for (const auto& note : held)
                    if (note.held && note.slot == 0 && (!latest || note.order > latest->order)) latest = &note;
                if (!latest) voice.gate = false;
                else if (voice.note_id == event.note_id) {
                    target_pitch(voice, latest->pitch, true);
                    voice.velocity = latest->velocity; voice.note_id = latest->note_id;
                }
            } else voice.gate = false;
        } else ledger->held = false;
        if (captures) captures->event(event, identity, "released");
    }

    bool settled(const DspState& item) const noexcept {
        return std::all_of(item.filters.begin(), item.filters.end(), [](const auto& filter) {
            return std::abs(filter.ic1) <= 1e-7 && std::abs(filter.ic2) <= 1e-7;
        });
    }
    bool distortion_settled(const DspState& item) const noexcept {
        return std::all_of(item.distortion.begin(), item.distortion.end(), [](const auto& distortion) {
            return std::abs(distortion.previous) <= 1e-7 && std::abs(distortion.wet) <= 1e-7;
        });
    }

    bool is_complete(std::uint32_t slot) noexcept {
        if (voices[slot].trigger) return false;
        if (!program.completion_root) return !voices[slot].gate && !mono; // Legacy direct-gate programs.
        for (std::size_t i = 0; i < program.completion.size(); ++i) {
            const auto& expression = program.completion[i];
            bool value = false;
            switch (expression.op) {
            case CompletionOp::gate_closed: value = !voices[slot].gate; break;
            case CompletionOp::envelope_idle: value = state(slot, expression.state_index).envelope.stage == Envelope::Stage::idle; break;
            case CompletionOp::filter_settled: value = settled(state(slot, expression.state_index)); break;
            case CompletionOp::distortion_settled: value = distortion_settled(state(slot, expression.state_index)); break;
            case CompletionOp::sample_done: {
                const auto& playback = state(slot, expression.state_index).sampler;
                value = playback.done || (!playback.initialized && !voices[slot].gate); break;
            }
            case CompletionOp::all:
                value = true; for (auto child : expression.inputs) value = value && completion_now[child]; break;
            case CompletionOp::any:
                for (auto child : expression.inputs) value = value || completion_now[child]; break;
            }
            completion_now[i] = value;
        }
        return completion_now[*program.completion_root] != 0;
    }

    void complete(std::uint32_t slot, std::uint64_t sample, CaptureSink* captures, bool trace) noexcept {
        auto& voice = voices[slot];
        if (!voice.sounding) return;
        voice.sounding = false; --stats.active_voices; ++stats.completed_notes;
        if (!mono) {
            voice.allocated = false; --stats.voice_instance_count;
            // A one-shot may finish while its key remains held. Later note_off
            // must never release another note that has reused this voice slot.
            for (auto& note : held) if (note.held && note.slot == slot) note.slot = no_slot;
        }
        if (trace && captures) {
            const Event event{sample, EventType::note_off, voice.note_id, voice.pitch, voice.velocity};
            captures->event(event, {slot, voice.generation, voice.note_id}, "completed");
        }
    }

    void complete_due(std::uint64_t sample, CaptureSink* captures, bool trace = true) noexcept {
        for (std::uint32_t slot = 0; slot < (mono ? 1U : config.max_voices); ++slot)
            if (voices[slot].allocated && voices[slot].sounding && is_complete(slot)) {
                if (boundary_length) boundaries[slot].mark();
                complete(slot, sample, captures, trace);
            }
    }

    void make_controls(Voice& voice, std::size_t count) noexcept {
        auto* pitch = controls.data(); auto* gate = pitch + config.max_block_size;
        auto* velocity = gate + config.max_block_size; auto* trigger = velocity + config.max_block_size;
        for (std::size_t i = 0; i < count; ++i) {
            pitch[i] = static_cast<float>(voice.pitch); gate[i] = voice.gate ? 1.0f : 0.0f;
            velocity[i] = static_cast<float>(voice.velocity); trigger[i] = i == 0 && voice.trigger ? 1.0f : 0.0f;
            if (voice.glide_elapsed < voice.glide_length) {
                ++voice.glide_elapsed;
                voice.pitch = voice.glide_start + (voice.glide_target - voice.glide_start) *
                    (double(voice.glide_elapsed) / double(voice.glide_length));
            }
        }
        voice.trigger = false;
    }

    void render_node(ValueId id, std::uint32_t slot, std::size_t count, std::uint64_t sample) noexcept {
        const auto& node = program.nodes[id];
        if (node.op == Op::parameter) return;
        if (node.op == Op::sampler) {
            auto& playback = state(slot, node.state_index).sampler;
            const auto& resource = *samples[node.resource_index];
            auto* left = data(id); auto* right = data(id, 1);
            auto* idle = state_idle.data() + node.state_index * config.max_block_size;
            const auto* trigger = data(node.inputs[0]); const auto* pitch = data(node.inputs[1]);
            const auto* root = data(node.inputs[2]); const auto* rate = data(node.inputs[3]);
            const auto* start = data(node.inputs[4]); const auto* end = data(node.inputs[5]);
            const auto* gain = data(node.inputs[6]); const auto* gate = data(node.inputs[7]);
            const bool loop = (node.flags & 1U) != 0, reverse = (node.flags & 2U) != 0;
            for (std::size_t i = 0; i < count; ++i) {
                left[i] = right[i] = 0.0f;
                if (!std::isfinite(trigger[i]) || !std::isfinite(pitch[i]) || !std::isfinite(root[i]) ||
                    !std::isfinite(rate[i]) || !std::isfinite(start[i]) || !std::isfinite(end[i]) ||
                    !std::isfinite(gain[i]) || !std::isfinite(gate[i])) {
                    left[i] = right[i] = std::numeric_limits<float>::quiet_NaN(); idle[i] = false; continue;
                }
                if (trigger[i] > 0.0f) {
                    const auto begin = std::clamp(double(start[i]), 0.0, 1.0);
                    const auto finish = std::clamp(double(end[i]), 0.0, 1.0);
                    playback.initialized = true;
                    playback.start = std::min(resource.left.size(), static_cast<std::size_t>(std::floor(begin * resource.left.size())));
                    playback.end = std::min(resource.left.size(), static_cast<std::size_t>(std::ceil(finish * resource.left.size())));
                    playback.done = begin >= finish || playback.start >= playback.end;
                    playback.position = reverse ? double(playback.end) - 1.0 : double(playback.start);
                }
                if (loop && playback.initialized && gate[i] <= 0.0f) playback.done = true;
                idle[i] = playback.done || (!playback.initialized && !voices[slot].gate);
                if (!playback.initialized || playback.done) continue;
                if (playback.previous_pitch != pitch[i] || playback.previous_root != root[i] || playback.previous_rate != rate[i]) {
                    playback.previous_pitch = pitch[i]; playback.previous_root = root[i]; playback.previous_rate = rate[i];
                    playback.increment = resource.sample_rate / config.sample_rate *
                        std::exp2((std::clamp(double(pitch[i]), 0.0, 127.0) - std::clamp(double(root[i]), 0.0, 127.0)) / 12.0) *
                        std::clamp(double(rate[i]), 1.0 / 64.0, 64.0);
                    const auto cutoff = std::clamp(4.0 * std::log2(std::max(1.0, playback.increment)), 0.0, double(sinc_cutoffs - 1));
                    playback.cutoff_row = std::min(sinc_cutoffs - 2, static_cast<std::size_t>(cutoff));
                    playback.cutoff_fraction = cutoff - double(playback.cutoff_row);
                }
                const auto increment = playback.increment;
                const auto index = static_cast<std::size_t>(std::floor(playback.position));
                const auto fraction = playback.position - double(index);
                if (fraction == 0.0 && increment <= 1.0) {
                    left[i] = resource.left[index] * gain[i];
                    right[i] = resource.right.empty() ? left[i] : resource.right[index] * gain[i];
                } else {
                    const auto phase = fraction * sinc_phases;
                    const auto phase_index = std::min(sinc_phases - 1, static_cast<std::size_t>(phase));
                    const auto phase_fraction = phase - double(phase_index);
                    const auto& c0p0 = (*sample_kernel)[playback.cutoff_row][phase_index];
                    const auto& c0p1 = (*sample_kernel)[playback.cutoff_row][phase_index + 1];
                    const auto& c1p0 = (*sample_kernel)[playback.cutoff_row + 1][phase_index];
                    const auto& c1p1 = (*sample_kernel)[playback.cutoff_row + 1][phase_index + 1];
                    double l = 0.0, r = 0.0;
                    const auto region_length = static_cast<std::int64_t>(playback.end - playback.start);
                    for (std::size_t tap = 0; tap < sinc_taps; ++tap) {
                        const auto w0 = double(c0p0[tap]) + (double(c0p1[tap]) - c0p0[tap]) * phase_fraction;
                        const auto w1 = double(c1p0[tap]) + (double(c1p1[tap]) - c1p0[tap]) * phase_fraction;
                        const auto weight = w0 + (w1 - w0) * playback.cutoff_fraction;
                        auto source_index = static_cast<std::int64_t>(index) + static_cast<std::int64_t>(tap) - 15;
                        if (source_index < static_cast<std::int64_t>(playback.start) || source_index >= static_cast<std::int64_t>(playback.end)) {
                            if (loop) {
                                auto offset = (source_index - static_cast<std::int64_t>(playback.start)) % region_length;
                                if (offset < 0) offset += region_length;
                                source_index = static_cast<std::int64_t>(playback.start) + offset;
                            } else source_index = std::clamp(source_index, static_cast<std::int64_t>(playback.start), static_cast<std::int64_t>(playback.end - 1));
                        }
                        l += resource.left[static_cast<std::size_t>(source_index)] * weight;
                        if (!resource.right.empty()) r += resource.right[static_cast<std::size_t>(source_index)] * weight;
                    }
                    left[i] = static_cast<float>(l * gain[i]);
                    right[i] = resource.right.empty() ? left[i] : static_cast<float>(r * gain[i]);
                }
                playback.position += reverse ? -increment : increment;
                if (playback.position < playback.start || playback.position >= playback.end) {
                    if (loop) {
                        const auto length = double(playback.end - playback.start);
                        auto offset = std::fmod(playback.position - double(playback.start), length);
                        if (offset < 0.0) offset += length;
                        // Rounding after wrapping a tiny reverse step can land
                        // exactly at end; keep the cursor inside the region.
                        playback.position = std::clamp(double(playback.start) + offset, double(playback.start),
                            std::nextafter(double(playback.end), double(playback.start)));
                    } else playback.done = true;
                }
            }
            return;
        }
        if (node.op == Op::unison_sine || node.op == Op::unison_saw) {
            auto& oscillator = state(slot, node.state_index);
            auto* left = data(id); auto* right = data(id, 1);
            const auto* frequency = data(node.inputs[0]); const auto* gain = data(node.inputs[1]);
            const auto* detune = data(node.inputs[2]); const auto* spread = data(node.inputs[3]);
            const auto* pan = data(node.inputs[4]); const auto* phase = data(node.inputs[5]);
            const auto* random = data(node.inputs[6]);
            if (!oscillator.oscillator_initialized || oscillator.oscillator_note != voices[slot].onset) {
                oscillator.oscillator_initialized = true; oscillator.oscillator_note = voices[slot].onset;
                for (std::uint32_t lane = 0; lane < node.flags; ++lane) {
                    const auto seed = voices[slot].note_id ^ (voices[slot].generation * 0xd6e8feb86659fd93ULL) ^
                        (std::uint64_t(node.state_index + 1) * 0xa0761d6478bd642fULL) ^ (std::uint64_t(lane + 1) * 0xe7037ed1a0b428dbULL);
                    oscillator.phases[lane] = wrap_phase(phase[0] + std::clamp(double(random[0]), 0.0, 1.0) * random_unit(seed));
                }
            }
            for (std::size_t i = 0; i < count; ++i) {
                double l = 0.0, r = 0.0;
                if (detune[i] != oscillator.previous_detune || spread[i] != oscillator.previous_spread || pan[i] != oscillator.previous_pan) {
                    for (std::uint32_t lane = 0; lane < node.flags; ++lane) {
                        const auto position = node.flags == 1 ? 0.0 : (2.0 * lane / (node.flags - 1) - 1.0);
                        oscillator.detune_ratio[lane] = std::exp2(position * detune[i] / 1200.0);
                        const auto angle = (std::clamp(double(pan[i]) + position * spread[i], -1.0, 1.0) + 1.0) * pi / 4.0;
                        oscillator.pan_left[lane] = 1.4142135623730951 * std::cos(angle);
                        oscillator.pan_right[lane] = 1.4142135623730951 * std::sin(angle);
                    }
                    oscillator.previous_detune = detune[i]; oscillator.previous_spread = spread[i]; oscillator.previous_pan = pan[i];
                }
                for (std::uint32_t lane = 0; lane < node.flags; ++lane) {
                    const auto hz = std::clamp(double(frequency[i]) * oscillator.detune_ratio[lane], 0.0, config.sample_rate * 0.49);
                    const auto delta = hz / config.sample_rate;
                    auto& current = oscillator.phases[lane];
                    const auto wave = node.op == Op::unison_sine ? std::sin(2.0 * pi * current) : 2.0 * current - 1.0 - blep(current, delta);
                    l += wave * oscillator.pan_left[lane];
                    r += wave * oscillator.pan_right[lane];
                    current = wrap_phase(current + delta);
                }
                left[i] = static_cast<float>(l * gain[i] / node.flags);
                right[i] = static_cast<float>(r * gain[i] / node.flags);
            }
            return;
        }
        if (node.op == Op::lfo) {
            auto& lfo = state(slot, node.state_index); auto* destination = data(id);
            const auto* rate = data(node.inputs[0]); const auto* trigger = data(node.inputs[1]);
            const auto* phase = data(node.inputs[2]); const auto* delay = data(node.inputs[3]);
            const auto* fade = data(node.inputs[4]);
            const auto shape = node.flags & 7U, mode = (node.flags >> 3U) & 3U;
            const bool bipolar = (node.flags & 32U) != 0, sync = (node.flags & 64U) != 0;
            for (std::size_t i = 0; i < count; ++i) {
                const auto frequency = sync ? transport.bpm / (240.0 * std::max(double(rate[i]), 1e-9)) : std::max(double(rate[i]), 0.0);
                const auto delta = std::min(frequency / config.sample_rate, 1.0);
                if (!std::isfinite(frequency) || !std::isfinite(phase[i])) {
                    destination[i] = std::numeric_limits<float>::quiet_NaN(); continue;
                }
                if (!lfo.lfo_initialized) {
                    lfo.lfo_initialized = true;
                    const auto cycles = double(phase[i]) + (mode == 0 ? double(sample + i) * delta : 0.0);
                    lfo.phase = wrap_phase(cycles); lfo.lfo_cycle = cycle_index(cycles);
                    lfo.lfo_elapsed = (mode == 0 || mode == 3) ? sample + i : 0;
                    lfo.lfo_done = mode == 2; lfo.lfo_seed = voices[slot].note_id;
                }
                if ((mode == 1 || mode == 2) && trigger[i] > 0.0f) {
                    lfo.phase = wrap_phase(phase[i]); lfo.lfo_cycle = 0;
                    lfo.lfo_elapsed = 0; lfo.lfo_done = false; lfo.lfo_seed = voices[slot].note_id;
                }
                if (mode == 3) {
                    const auto beats = ppq_at(sample + i);
                    const auto cycles = double(phase[i]) + (sync ? beats / (4.0 * std::max(double(rate[i]), 1e-9)) : beats * 60.0 * frequency / transport.bpm);
                    if (!std::isfinite(cycles)) { destination[i] = std::numeric_limits<float>::quiet_NaN(); continue; }
                    lfo.phase = wrap_phase(cycles); lfo.lfo_cycle = cycle_index(cycles);
                }
                const auto delay_samples = samples_for(delay[i], config.sample_rate);
                const auto fade_samples = samples_for(fade[i], config.sample_rate);
                if (lfo.lfo_elapsed < delay_samples) destination[i] = 0.0f;
                else {
                    const auto seed = std::uint64_t(node.state_index + 1) * 0xa0761d6478bd642fULL ^
                        ((mode == 0 || mode == 3) ? 0 : lfo.lfo_seed);
                    const auto wave = lfo_wave(lfo.phase, lfo.lfo_cycle, shape, seed);
                    const auto ramp = fade_samples == 0 ? 1.0 : std::min(1.0, double(lfo.lfo_elapsed - delay_samples) / fade_samples);
                    destination[i] = static_cast<float>((bipolar ? wave : 0.5 * (wave + 1.0)) * ramp);
                    if (mode != 3 && !(mode == 2 && lfo.lfo_done)) {
                        lfo.phase += delta;
                        if (mode == 2 && lfo.phase >= 1.0) { lfo.phase = 1.0; lfo.lfo_done = true; }
                        else if (lfo.phase >= 1.0) { const auto cycles = std::floor(lfo.phase); lfo.phase -= cycles; lfo.lfo_cycle += static_cast<std::int64_t>(cycles); }
                    }
                }
                if (mode != 3 || transport.playing) ++lfo.lfo_elapsed;
            }
            return;
        }
        for (std::uint32_t channel = 0; channel < node.channels; ++channel) {
        auto* destination = data(id, channel);
        const float* a = node.inputs.empty() ? nullptr : data(node.inputs[0], channel);
        const float* b = node.inputs.size() < 2 ? nullptr : data(node.inputs[1], channel);
        const float* c = node.inputs.size() < 3 ? nullptr : data(node.inputs[2], channel);
        switch (node.op) {
        case Op::parameter: break;
        case Op::key_pitch: std::copy_n(controls.data(), count, destination); break;
        case Op::key_gate: std::copy_n(controls.data() + config.max_block_size, count, destination); break;
        case Op::key_velocity: std::copy_n(controls.data() + 2 * config.max_block_size, count, destination); break;
        case Op::key_trigger: std::copy_n(controls.data() + 3 * config.max_block_size, count, destination); break;
        case Op::pitch_to_hz: {
            float previous = a[0], converted = static_cast<float>(440.0 * std::exp2((previous - 69.0) / 12.0));
            for (std::size_t i = 0; i < count; ++i) {
                if (a[i] != previous) { previous = a[i]; converted = static_cast<float>(440.0 * std::exp2((previous - 69.0) / 12.0)); }
                destination[i] = converted;
            }
            break;
        }
        case Op::db_to_linear: {
            float previous = a[0], converted = static_cast<float>(std::pow(10.0, previous / 20.0));
            for (std::size_t i = 0; i < count; ++i) {
                if (a[i] != previous) { previous = a[i]; converted = static_cast<float>(std::pow(10.0, previous / 20.0)); }
                destination[i] = converted;
            }
            break;
        }
        case Op::phasor: {
            auto& phase = state(slot, node.state_index).phase;
            for (std::size_t i = 0; i < count; ++i) {
                destination[i] = static_cast<float>(phase);
                const auto frequency = std::isfinite(a[i]) ? std::clamp(double(a[i]), 0.0, config.sample_rate * 0.49) : 0.0;
                phase += frequency / config.sample_rate; phase -= std::floor(phase);
            }
            break;
        }
        case Op::math_sin: case Op::math_cos:
            for (std::size_t i = 0; i < count; ++i)
                destination[i] = std::isfinite(a[i]) ? (node.op == Op::math_sin ? std::sin(a[i]) : std::cos(a[i])) : 0.0f;
            break;
        case Op::sine: case Op::saw: {
            auto& phase = state(slot, node.state_index).phase;
            for (std::size_t i = 0; i < count; ++i) {
                const auto frequency = std::clamp(double(a[i]), 0.0, config.sample_rate * 0.49);
                const auto delta = frequency / config.sample_rate;
                const auto wave = node.op == Op::sine ? std::sin(2.0 * pi * phase) : 2.0 * phase - 1.0 - blep(phase, delta);
                destination[i] = static_cast<float>(wave * b[i]); phase += delta; phase -= std::floor(phase);
            }
            break;
        }
        case Op::adsr: {
            auto& envelope = state(slot, node.state_index).envelope;
            auto* idle = state_idle.data() + node.state_index * config.max_block_size;
            const auto* decay = data(node.inputs[3]); const auto* sustain = data(node.inputs[4]); const auto* release = data(node.inputs[5]);
            for (std::size_t i = 0; i < count; ++i) {
                bool is_idle = false;
                destination[i] = envelope.tick(a[i] > 0, b[i] > 0, (node.flags & retrigger_from_zero) != 0,
                    c[i], decay[i], sustain[i], release[i], config.sample_rate, is_idle);
                idle[i] = is_idle;
            }
            break;
        }
        case Op::filter_lowpass: case Op::filter_highpass: {
            auto& filter = state(slot, node.state_index);
            auto* idle = state_idle.data() + node.state_index * config.max_block_size;
            for (std::size_t i = 0; i < count; ++i) {
                auto& component = filter.filters[channel];
                destination[i] = filter_tick(component, a[i], b[i], c[i], config.sample_rate, node.op == Op::filter_highpass);
                const bool is_idle = std::abs(component.ic1) <= 1e-7 && std::abs(component.ic2) <= 1e-7;
                idle[i] = channel == 0 ? is_idle : idle[i] && is_idle;
            }
            break;
        }
        case Op::distortion: {
            auto& memory = state(slot, node.state_index).distortion[channel];
            auto* idle = state_idle.data() + node.state_index * config.max_block_size;
            const auto* gain = data(node.inputs[3], channel);
            for (std::size_t i = 0; i < count; ++i) {
                destination[i] = distortion_tick(memory, a[i], b[i], c[i], gain[i], node.flags);
                const bool is_idle = std::abs(memory.previous) <= 1e-7 && std::abs(memory.wet) <= 1e-7;
                idle[i] = channel == 0 ? is_idle : idle[i] && is_idle;
            }
            break;
        }
        case Op::stereo: std::copy_n(data(node.inputs[channel]), count, destination); break;
        case Op::left: std::copy_n(data(node.inputs[0]), count, destination); break;
        case Op::right: std::copy_n(data(node.inputs[0], 1), count, destination); break;
        case Op::mid: case Op::side: {
            const auto* right = data(node.inputs[0], 1);
            for (std::size_t i = 0; i < count; ++i) destination[i] = 0.5f * (a[i] + (node.op == Op::mid ? right[i] : -right[i]));
            break;
        }
        case Op::unison_sine: case Op::unison_saw: case Op::lfo: case Op::sampler: break;
        case Op::add: for (std::size_t i = 0; i < count; ++i) destination[i] = a[i] + b[i]; break;
        case Op::subtract: for (std::size_t i = 0; i < count; ++i) destination[i] = a[i] - b[i]; break;
        case Op::multiply: for (std::size_t i = 0; i < count; ++i) destination[i] = a[i] * b[i]; break;
        case Op::divide: for (std::size_t i = 0; i < count; ++i) destination[i] = a[i] / b[i]; break;
        case Op::less: for (std::size_t i = 0; i < count; ++i) destination[i] = a[i] < b[i]; break;
        case Op::less_equal: for (std::size_t i = 0; i < count; ++i) destination[i] = a[i] <= b[i]; break;
        case Op::greater: for (std::size_t i = 0; i < count; ++i) destination[i] = a[i] > b[i]; break;
        case Op::greater_equal: for (std::size_t i = 0; i < count; ++i) destination[i] = a[i] >= b[i]; break;
        case Op::equal: for (std::size_t i = 0; i < count; ++i) destination[i] = a[i] == b[i]; break;
        case Op::not_equal: for (std::size_t i = 0; i < count; ++i) destination[i] = a[i] != b[i]; break;
        case Op::select: for (std::size_t i = 0; i < count; ++i) destination[i] = a[i] != 0.0f ? b[i] : c[i]; break;
        }
        }
    }

    std::size_t completion_offset(std::uint32_t slot, std::size_t count) noexcept {
        if (!voices[slot].sounding || !program.completion_root) return count;
        for (std::size_t id = 0; id < program.completion.size(); ++id) {
            const auto& expression = program.completion[id];
            auto* result = completion_samples.data() + id * config.max_block_size;
            const auto* idle = state_idle.data() + expression.state_index * config.max_block_size;
            for (std::size_t i = 0; i < count; ++i) {
                bool value = false;
                switch (expression.op) {
                case CompletionOp::gate_closed: value = !voices[slot].gate; break;
                case CompletionOp::envelope_idle: case CompletionOp::filter_settled: case CompletionOp::distortion_settled: case CompletionOp::sample_done: value = idle[i]; break;
                case CompletionOp::all:
                    value = true; for (auto child : expression.inputs) value = value && completion_samples[child * config.max_block_size + i]; break;
                case CompletionOp::any:
                    for (auto child : expression.inputs) value = value || completion_samples[child * config.max_block_size + i]; break;
                }
                result[i] = value;
            }
        }
        const auto* root = completion_samples.data() + *program.completion_root * config.max_block_size;
        for (std::size_t i = 0; i < count; ++i) if (root[i]) return i;
        return count;
    }

    void render(std::span<float> output, std::span<float> right, std::uint64_t sample, CaptureSink* captures) noexcept {
        if (output.empty()) return;
        make_parameters(output.size());
        for (std::uint32_t slot = 0; slot < (mono ? 1U : config.max_voices); ++slot) {
            auto& voice = voices[slot];
            auto& boundary = boundaries[slot];
            update_output_gain(voice);
            if (!voice.allocated) {
                // A finished slot can be reused immediately; its bounded correction
                // survives independently of the voice ledger and DSP state.
                if (boundary_length && boundary.active()) for (std::size_t i = 0; i < output.size(); ++i) {
                    const auto tail = boundary.tick(0, 0, boundary_length);
                    const auto fader=output_gain(voice);
                    output[i] += fader*(right.empty() ? 0.5f * (tail[0] + tail[1]) : tail[0]);
                    if (!right.empty()) right[i] += fader*tail[1];
                }
                continue;
            }
            make_controls(voice, output.size());
            for (ValueId id = 0; id < program.nodes.size(); ++id) render_node(id, slot, output.size(), sample);
            const auto complete_at = completion_offset(slot, output.size());
            const auto observed_count = mono ? output.size() : std::min(output.size(), complete_at + 1);
            // Scratch has no aliases in this backend. Capture these same computed values
            // before the next voice reuses it; no node is ever evaluated twice for probes.
            if (captures) for (ValueId id = 0; id < program.nodes.size(); ++id) {
                if (!named_taps[id]) continue;
                const VoiceIdentity identity{slot, voice.generation, voice.note_id};
                if (program.nodes[id].channels == 2)
                    captures->capture_stereo(id, identity, sample, {data(id), observed_count}, {data(id, 1), observed_count});
                else captures->capture(id, identity, sample, {data(id), observed_count});
            }
            const auto* signal = data(program.output); const auto* other = data(program.output, 1);
            const bool stereo_source = program.nodes[program.output].channels == 2;
            for (std::size_t i = 0; i < output.size(); ++i) {
                const bool audible = mono || i < complete_at;
                const auto fader=output_gain(voice);
                if (boundary_length) {
                    if (i == complete_at) boundary.mark();
                    // Bandlimited oscillators can start at zero yet jump on the
                    // following sample. A 1 ms onset window covers that edge too.
                    float gain = 1;
                    if (voice.sounding && voice.attack_elapsed < attack_length) {
                        const double t = double(voice.attack_elapsed++) / attack_length;
                        gain = static_cast<float>(t * t * (3 - 2 * t));
                    }
                    const auto audio = boundary.tick(audible ? signal[i] * gain : 0, audible ? other[i] * gain : 0, boundary_length);
                    output[i] += fader*(right.empty() ? 0.5f * (audio[0] + audio[1]) : audio[0]);
                    if (!right.empty()) right[i] += fader*audio[1];
                } else if (audible) {
                    if (!right.empty()) { output[i] += fader*signal[i]; right[i] += fader*other[i]; }
                    else output[i] += fader*(stereo_source ? 0.5f * (signal[i] + other[i]) : signal[i]);
                }
            }
            if (complete_at < output.size()) complete(slot, sample + complete_at, captures, true);
        }
        complete_due(sample + output.size(), captures);
    }

    ProcessResult process(std::span<const Event> events, std::span<float> output, std::span<float> right,
                          CaptureSink* captures, std::span<const ParameterUpdate> updates) noexcept {
        const auto error = !right.empty() && (right.size() != output.size() || right.data() == output.data())
            ? ProcessError::block_capacity : validate(events, updates, output.size());
        std::fill(output.begin(), output.end(), 0.0f);
        std::fill(right.begin(), right.end(), 0.0f);
        if (error != ProcessError::none) {
            panic(); ++stats.process_errors; stats.processed_samples += output.size(); return {error};
        }
        std::size_t cursor = 0, event_cursor = 0, parameter_cursor = 0;
        while (event_cursor < events.size() || parameter_cursor < updates.size()) {
            const auto event_sample = event_cursor < events.size() ? events[event_cursor].sample : std::numeric_limits<std::uint64_t>::max();
            const auto parameter_sample = parameter_cursor < updates.size() ? updates[parameter_cursor].sample : std::numeric_limits<std::uint64_t>::max();
            const auto sample = std::min(event_sample, parameter_sample);
            const auto offset = static_cast<std::size_t>(sample - stats.processed_samples);
            render(output.subspan(cursor, offset - cursor), right.empty() ? right : right.subspan(cursor, offset - cursor), stats.processed_samples + cursor, captures);
            while (parameter_cursor < updates.size() && updates[parameter_cursor].sample == sample) parameter(updates[parameter_cursor++]);
            while (event_cursor < events.size() && events[event_cursor].sample == sample) apply(events[event_cursor++], captures);
            // Direct gate completion is immediate; envelope completion waits for its release.
            complete_due(sample, captures, false);
            cursor = offset;
        }
        render(output.subspan(cursor), right.empty() ? right : right.subspan(cursor), stats.processed_samples + cursor, captures);
        stats.processed_samples += output.size();
        const auto nonfinite = [](float sample) { return !std::isfinite(sample); };
        if (std::any_of(output.begin(), output.end(), nonfinite) || std::any_of(right.begin(), right.end(), nonfinite)) {
            panic(); std::fill(output.begin(), output.end(), 0.0f); std::fill(right.begin(), right.end(), 0.0f); ++stats.process_errors;
            return {ProcessError::nonfinite_output};
        }
        return {};
    }

    ProcessResult restore_held_notes(std::span<const Event> notes, CaptureSink* captures,
                                     std::span<const ParameterUpdate> initial_parameters) noexcept {
        auto error = notes.size() > held_limit ? ProcessError::held_capacity : ProcessError::none;
        if (error == ProcessError::none) error = validate({}, initial_parameters, 1);
        if (std::any_of(held.begin(), held.end(), [](const HeldNote& note) { return note.held; }) || stats.active_voices != 0)
            error = ProcessError::duplicate_note;
        for (std::size_t i = 0; error == ProcessError::none && i < notes.size(); ++i) {
            const auto& note = notes[i];
            if (note.type != EventType::note_on || note.sample != stats.processed_samples ||
                !std::isfinite(note.pitch) || note.pitch < 0 || note.pitch > 127 ||
                !std::isfinite(note.velocity) || note.velocity < 0 || note.velocity > 1)
                error = ProcessError::invalid_note;
            for (std::size_t j = 0; j < i; ++j)
                if (notes[j].note_id == note.note_id) { error = ProcessError::duplicate_note; break; }
        }
        if (error != ProcessError::none) { panic(); ++stats.process_errors; return {error}; }
        for (const auto& update : initial_parameters) parameter(update);
        for (const auto& note : notes) apply(note, captures);
        return {};
    }
};

Engine::Engine(const Program& program, EngineConfig config, SampleBank samples) : impl_(std::make_unique<Impl>(program, config, std::move(samples))) {}
Engine::~Engine() = default;
ProcessResult Engine::process(std::span<const Event> events, std::span<float> output, CaptureSink* captures,
                             std::span<const ParameterUpdate> parameters) noexcept {
    return impl_->process(events, output, {}, captures, parameters);
}
ProcessResult Engine::process_stereo(std::span<const Event> events, std::span<float> left,
                                    std::span<float> right, CaptureSink* captures,
                                    std::span<const ParameterUpdate> parameters) noexcept {
    if (left.size() != right.size()) {
        impl_->panic(); std::fill(left.begin(), left.end(), 0.0f); std::fill(right.begin(), right.end(), 0.0f);
        ++impl_->stats.process_errors; impl_->stats.processed_samples += left.size();
        return {ProcessError::block_capacity};
    }
    return impl_->process(events, left, right, captures, parameters);
}
void Engine::set_transport(Transport transport) noexcept { impl_->set_transport(transport); }
void CaptureSink::capture_stereo(ValueId value, VoiceIdentity voice, std::uint64_t start_sample,
                                std::span<const float> left, std::span<const float> right) noexcept {
    std::array<float, 256> mixed{};
    const auto size = std::min(left.size(), right.size());
    for (std::size_t offset = 0; offset < size; offset += mixed.size()) {
        const auto count = std::min(mixed.size(), size - offset);
        for (std::size_t i = 0; i < count; ++i) mixed[i] = 0.5f * (left[offset + i] + right[offset + i]);
        capture(value, voice, start_sample + offset, std::span<const float>(mixed).first(count));
    }
}
void Engine::set_output_gains(std::span<const OutputGainGroup> groups,float fallback) noexcept {
    impl_->output_group_count=std::min(groups.size(),impl_->output_groups.size());
    impl_->default_output_gain=std::isfinite(fallback)?std::clamp(fallback,0.f,2.f):0.f;
    for(std::size_t i=0;i<impl_->output_group_count;++i){impl_->output_groups[i]=groups[i];impl_->output_groups[i].gain=std::isfinite(groups[i].gain)?std::clamp(groups[i].gain,0.f,2.f):0.f;}
}
void Engine::panic() noexcept { impl_->panic(); }
void Engine::set_capture_values(std::span<const ValueId> values) noexcept {
    std::fill(impl_->named_taps.begin(), impl_->named_taps.end(), 0);
    for (auto value : values) if (value < impl_->named_taps.size()) impl_->named_taps[value] = 1;
}
ProcessResult Engine::restore_held_notes(std::span<const Event> notes, CaptureSink* captures,
                                       std::span<const ParameterUpdate> initial_parameters) noexcept {
    return impl_->restore_held_notes(notes, captures, initial_parameters);
}
const EngineStats& Engine::stats() const noexcept { return impl_->stats; }
} // namespace flows
