#include "engine/engine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
bool count_heap = false;
std::size_t allocation_count = 0;
std::size_t deallocation_count = 0;
}

void* operator new(std::size_t size) {
    if (count_heap) ++allocation_count;
    if (void* result = std::malloc(size == 0 ? 1 : size)) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept {
    if (count_heap && pointer) ++deallocation_count;
    std::free(pointer);
}
void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { ::operator delete(pointer); }

namespace {
using namespace flows;
constexpr double tau = 6.283185307179586476925286766559;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void rejects(Function&& function, const char* message) {
    bool rejected = false;
    try { function(); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, message);
}

Node node(Op op, std::initializer_list<ValueId> inputs = {}, double value = 0.0,
          std::uint32_t state = 0) {
    Node result;
    result.op = op;
    result.inputs = inputs;
    result.initial_value = value;
    result.state_index = state;
    return result;
}

Program sine_program() {
    Program result;
    result.states.push_back({"osc", {}});
    result.nodes = {
        node(Op::key_pitch), node(Op::key_gate), node(Op::key_velocity),
        node(Op::parameter, {}, -6.0), node(Op::pitch_to_hz, {0}),
        node(Op::db_to_linear, {3}), node(Op::sine, {4, 5}),
        node(Op::multiply, {6, 1}), node(Op::multiply, {7, 2})
    };
    result.named_values = {{"raw", 6, {}}, {"raw_alias", 6, {}}, {"gated", 7, {}}};
    result.output = 8;
    return result;
}

Program envelope_program(bool mono = false) {
    auto result = sine_program();
    result.states.push_back({"amp_env", {}, StateKind::envelope});
    result.nodes.push_back(node(Op::key_trigger)); // 9
    result.nodes.push_back(node(Op::parameter, {}, 0.01)); // 10 A
    result.nodes.push_back(node(Op::parameter, {}, 0.1)); // 11 D
    result.nodes.push_back(node(Op::parameter, {}, 0.6)); // 12 S
    result.nodes.push_back(node(Op::parameter, {}, 0.3)); // 13 R
    result.nodes.push_back(node(Op::adsr, {1, 9, 10, 11, 12, 13}, 0.0, 1)); // 14
    result.nodes.push_back(node(Op::multiply, {6, 14})); // 15
    result.nodes.push_back(node(Op::multiply, {15, 2})); // 16
    result.output = 16;
    result.voice_policy.mode = mono ? VoiceMode::mono : VoiceMode::poly;
    result.completion = {{CompletionOp::envelope_idle, 1, {}}};
    result.completion_root = 0;
    result.named_values = {{"key.pitch", 0, {}}, {"key.gate", 1, {}}, {"key.velocity", 2, {}},
                           {"key.trigger", 9, {}}, {"raw", 6, {}}, {"amp", 14, {}}};
    return result;
}

struct Signals final : CaptureSink {
    std::vector<std::vector<float>> values;
    std::uint64_t selected_note = 0;
    std::array<std::uint64_t, 64> completion_times{};
    std::size_t completions = 0;
    explicit Signals(std::size_t nodes, std::size_t frames) : values(nodes, std::vector<float>(frames, -999.0f)) {}
    void capture(ValueId value, VoiceIdentity voice, std::uint64_t sample, std::span<const float> samples) noexcept override {
        if (selected_note != 0 && selected_note != voice.note_id) return;
        if (value >= values.size() || sample + samples.size() > values[value].size()) return;
        std::copy(samples.begin(), samples.end(), values[value].begin() + static_cast<std::ptrdiff_t>(sample));
    }
    void event(const Event& event_value, VoiceIdentity, std::string_view action) noexcept override {
        if (action == "completed" && completions < completion_times.size()) completion_times[completions++] = event_value.sample;
    }
};

void near(float actual, double expected, const char* message, double tolerance = 2e-6) {
    if (std::abs(double(actual) - expected) > tolerance) {
        std::cerr << message << ": actual=" << actual << ", expected=" << expected << '\n';
        check(false, message);
    }
}

Event on(std::uint64_t time, std::uint64_t id, double pitch = 69.0, double velocity = 1.0) {
    return {time, EventType::note_on, id, pitch, velocity};
}
Event off(std::uint64_t time, std::uint64_t id) {
    return {time, EventType::note_off, id, 0.0, 0.0};
}

struct FixedCapture final : CaptureSink {
    struct Record { Event event; VoiceIdentity voice; std::string_view action; };
    std::array<Record, 64> events{};
    std::size_t event_count = 0;
    std::size_t capture_count = 0;
    std::size_t raw_samples = 0;
    double raw_energy = 0.0;
    std::uint64_t first_sample = std::numeric_limits<std::uint64_t>::max();
    void capture(ValueId value, VoiceIdentity, std::uint64_t sample,
                 std::span<const float> samples) noexcept override {
        ++capture_count;
        first_sample = std::min(first_sample, sample);
        if (value == 6) {
            raw_samples += samples.size();
            for (const auto value_sample : samples) raw_energy += double(value_sample) * value_sample;
        }
    }
    void event(const Event& event_value, VoiceIdentity voice,
               std::string_view action) noexcept override {
        if (event_count < events.size()) events[event_count] = {event_value, voice, action};
        ++event_count;
    }
};

std::vector<float> render(const Program& program, std::span<const Event> events,
                          std::size_t frames, std::span<const std::size_t> blocks,
                          CaptureSink* capture = nullptr, std::uint32_t voices = 32,
                          std::span<const ParameterUpdate> updates = {}) {
    Engine engine(program, {48000.0, 256, voices});
    std::vector<float> result(frames);
    std::size_t cursor = 0;
    std::size_t event_cursor = 0;
    std::size_t block_cursor = 0;
    std::size_t parameter_cursor = 0;
    while (cursor < frames) {
        const auto count = std::min(blocks[block_cursor++ % blocks.size()], frames - cursor);
        auto event_end = event_cursor;
        while (event_end < events.size() && events[event_end].sample < cursor + count) ++event_end;
        auto parameter_end = parameter_cursor;
        while (parameter_end < updates.size() && updates[parameter_end].sample < cursor + count) ++parameter_end;
        check(static_cast<bool>(engine.process(events.subspan(event_cursor, event_end - event_cursor),
                       std::span<float>(result).subspan(cursor, count), capture,
                       updates.subspan(parameter_cursor, parameter_end - parameter_cursor))), "render failed");
        cursor += count;
        event_cursor = event_end;
        parameter_cursor = parameter_end;
    }
    return result;
}

void analytic_reference() {
    const std::array events{on(17, 1), off(4800, 1)};
    const std::array<std::size_t, 1> blocks{64};
    const auto audio = render(sine_program(), events, 5000, blocks);
    double max_error = 0.0;
    for (std::size_t sample = 0; sample < audio.size(); ++sample) {
        const double expected = sample >= 17 && sample < 4800 ?
            std::sin(tau * 440.0 * double(sample - 17) / 48000.0) * std::pow(10.0, -6.0 / 20.0) : 0.0;
        max_error = std::max(max_error, std::abs(audio[sample] - expected));
    }
    check(max_error < 1.0e-7, "A4 differs from the independent analytic sine reference");
    check(audio[17] == 0.0f && audio[4800] == 0.0f, "note boundary timing is incorrect");
}

void partition_invariance() {
    const std::array events{on(0, 1, 60.0, 0.6), on(64, 2, 72.0, 0.2), off(129, 1),
                            on(129, 3, 67.0, 0.7), off(2049, 2), off(6079, 3)};
    const std::array<std::size_t, 1> singles{1}, blocks64{64}, blocks127{127};
    std::array<std::size_t, 79> random_blocks{};
    std::uint32_t random = 0x4d595df4;
    for (auto& size : random_blocks) { random = random * 1664525 + 1013904223; size = random % 256 + 1; }
    const auto single = render(sine_program(), events, 8192, singles);
    check(single == render(sine_program(), events, 8192, blocks64), "64-sample blocks change the signal");
    check(single == render(sine_program(), events, 8192, blocks127), "127-sample blocks change the signal");
    check(single == render(sine_program(), events, 8192, random_blocks), "random block partitions change the signal");
}

void note_identity_and_phase() {
    const std::array events{on(0, 100), on(10, 200), off(100, 100), off(200, 200),
                            on(210, 300), off(250, 300)};
    const std::array<std::size_t, 1> blocks{64};
    FixedCapture capture;
    const auto audio = render(sine_program(), events, 300, blocks, &capture);
    const auto gain = std::pow(10.0, -6.0 / 20.0);
    const auto expected = std::sin(tau * 440.0 * 90.0 / 48000.0) * gain;
    check(std::abs(audio[100] - expected) < 1e-7, "same-pitch note IDs release the wrong voice");
    check(audio[210] == 0.0f, "reused voice slot did not reset phase");
    check(capture.event_count == 6, "missing event trace");
    check(capture.events[0].voice.slot == capture.events[4].voice.slot &&
          capture.events[0].voice.generation + 1 == capture.events[4].voice.generation,
          "slot reuse did not update voice generation");
    check(capture.events[2].voice.note_id == 100 && capture.events[3].voice.note_id == 200,
          "event trace lost note identity");
}

void independent_oscillator_state() {
    auto program = sine_program();
    program.states.push_back({"second_osc", {}});
    program.nodes.push_back(node(Op::sine, {4, 5}, 0.0, 1));
    program.nodes.push_back(node(Op::add, {6, 9}));
    program.output = 10;
    const std::array events{on(0, 1)};
    const std::array<std::size_t, 1> blocks{64};
    const auto one = render(sine_program(), events, 256, blocks);
    const auto two = render(program, events, 256, blocks);
    for (std::size_t i = 0; i < one.size(); ++i)
        check(two[i] == one[i] * 2.0f, "two oscillator states shared phase memory");
}

void capture_is_observational() {
    const std::array events{on(13, 1), off(179, 1)};
    const std::array<std::size_t, 1> blocks{64};
    FixedCapture capture;
    const auto without = render(sine_program(), events, 256, blocks);
    const auto with = render(sine_program(), events, 256, blocks, &capture);
    check(without == with, "capture changed the audio output");
    check(capture.raw_samples == 166, "named alias was captured twice or samples were lost");
    check(capture.first_sample == 13 && capture.raw_energy > 0.0, "capture timing/data are incorrect");

    Engine switched(sine_program(), {48000.0, 64, 32});
    Engine reference(sine_program(), {48000.0, 64, 32});
    const std::array start{on(0, 1)};
    std::array<float, 64> a{}, b{};
    switched.process(start, a);
    reference.process(start, b);
    switched.process({}, a, &capture);
    reference.process({}, b);
    check(a == b, "opening capture restarted oscillator state");
    switched.process({}, a);
    reference.process({}, b);
    check(a == b, "closing capture restarted oscillator state");
}

void bounded_voice_admission() {
    Engine engine(sine_program(), {48000.0, 64, 2});
    const std::array events{on(0, 1), on(0, 2), on(0, 3), off(10, 1),
                            off(20, 3), on(30, 4), off(40, 2), off(50, 4)};
    std::array<float, 64> audio{};
    FixedCapture capture;
    engine.process(events, audio, &capture);
    check(engine.stats().accepted_notes == 3 && engine.stats().rejected_notes == 1 &&
          engine.stats().completed_notes == 3 && engine.stats().active_voices == 0,
          "voice admission/completion counts are wrong");
    check(capture.events[2].action == "rejected" &&
          capture.events[2].voice.slot == std::numeric_limits<std::uint32_t>::max(),
          "rejected voice did not carry an invalid slot");
    check(capture.events[4].voice.slot == std::numeric_limits<std::uint32_t>::max(),
          "rejected held note was unexpectedly promoted");
    check(audio[50] == 0.0f, "completed voices remained audible");
}

void validation_is_atomic() {
    auto program = sine_program();
    rejects([&] { Engine bad(program, {0.0, 64, 32}); }, "zero sample rate accepted");
    rejects([&] { Engine bad(program, {48000.0, 0, 32}); }, "zero block capacity accepted");
    rejects([&] { Engine bad(program, {48000.0, 64, 33}); }, "voice capacity above 32 accepted");
    auto invalid = program;
    invalid.nodes[4].inputs[0] = 8;
    rejects([&] { Engine bad(invalid, {}); }, "non-topological DSP graph accepted");
    invalid = program;
    invalid.nodes.push_back(node(Op::sine, {4, 5}));
    rejects([&] { Engine bad(invalid, {}); }, "duplicate stateful producer accepted");

    Engine engine(program, {48000.0, 64, 32});
    std::array<float, 64> audio{};
    audio.fill(0.25f);
    const std::array duplicate{on(0, 1), on(10, 1)};
    check(engine.process(duplicate, audio).error == ProcessError::duplicate_note, "duplicate held ID accepted");
    check(engine.stats().accepted_notes == 0 && engine.stats().processed_samples == 64 &&
          audio[0] == 0.0f, "invalid block did not panic, silence and advance clock");
    const std::array outside{on(128, 1)};
    check(engine.process(outside, audio).error == ProcessError::event_range, "event at next block boundary accepted early");
    const std::array unsorted{on(130, 1), off(129, 1)};
    check(engine.process(unsorted, audio).error == ProcessError::event_order, "unsorted events accepted");
    const std::array unknown{off(192, 1)};
    check(engine.process(unknown, audio).error == ProcessError::unknown_note, "unmatched note-off accepted");
    std::array<Event, 257> excessive{};
    check(engine.process(excessive, audio).error == ProcessError::event_capacity, "more than 256 events per block accepted");
    engine.process({}, {});
    check(engine.stats().processed_samples == 320, "zero-frame block changed sample clock");
    const std::array start{on(320, 1)};
    engine.process(start, audio);
    check(engine.stats().accepted_notes == 1 && audio[1] != 0.0f, "engine did not recover after rejected block");
}

void held_ledger_capacity() {
    Engine engine(sine_program(), {48000.0, 1, 1});
    std::array<Event, 256> batch{};
    std::array<float, 1> audio{};
    for (std::uint64_t group = 0; group < 4; ++group) {
        for (std::size_t i = 0; i < batch.size(); ++i) batch[i] = on(group, group * 256 + i);
        engine.process(batch, audio);
    }
    check(engine.stats().accepted_notes == 1 && engine.stats().rejected_notes == 1023,
          "rejected notes were not retained in held ledger");
    const std::array overflow{on(4, 1024)};
    check(engine.process(overflow, audio).error == ProcessError::held_capacity, "held ledger overflow was accepted");
    check(engine.stats().processed_samples == 5 && engine.stats().active_voices == 0, "ledger overflow did not panic and advance clock");
    const std::array after_panic{on(5, 1024)};
    engine.process(after_panic, audio);
    check(engine.stats().accepted_notes == 2, "ledger entries were not reusable after panic");
}

void no_heap_in_valid_process() {
    Engine engine(sine_program(), {48000.0, 64, 2});
    const std::array events{on(0, 1), on(4, 2), on(8, 3), off(20, 3), off(32, 1), off(63, 2)};
    std::array<float, 64> audio{};
    FixedCapture capture;
    allocation_count = 0;
    deallocation_count = 0;
    count_heap = true;
    engine.process(events, audio, &capture);
    engine.process({}, audio, &capture);
    engine.process({}, {});
    count_heap = false;
    check(allocation_count == 0 && deallocation_count == 0,
          "valid process path allocated or freed C++ heap memory");
}

void envelope_timing_and_retrigger() {
    const std::array<std::size_t, 1> blocks{64};
    auto program = envelope_program(true);
    const std::array events{on(0, 1), on(24000, 2, 72), off(48000, 2), off(72000, 1)};
    Signals signals(program.nodes.size(), 90000);
    render(program, events, 90000, blocks, &signals);
    const auto& amp = signals.values[14];
    near(amp[0], 0, "Attack start"); near(amp[240], 0.5, "Attack midpoint");
    near(amp[480], 1, "Attack full duration"); near(amp[5280], 0.6, "Decay duration");
    near(amp[24000], 0.6, "current retrigger starts at existing value");
    near(amp[24240], 0.8, "current retrigger uses full Attack duration");
    near(amp[24480], 1, "current retrigger reaches peak");
    near(amp[48000], 0.6, "last-key fallback must not retrigger envelope");
    near(amp[72000], 0.6, "Release starts at current value");
    near(amp[79200], 0.3, "Release midpoint"); near(amp[86400], 0, "Release endpoint");
    near(signals.values[9][24000], 1, "each_press trigger"); near(signals.values[9][24001], 0, "trigger is one sample");
    near(signals.values[9][48000], 0, "fallback has no trigger");
    near(signals.values[0][48000], 69, "fallback pitch");

    program.nodes[14].flags = retrigger_from_zero;
    Signals zero(program.nodes.size(), 25000);
    const std::array starts{on(0, 1), on(24000, 2, 72)};
    render(program, starts, 25000, blocks, &zero);
    near(zero.values[14][24000], 0, "zero retrigger starts at zero");
    near(zero.values[14][24240], 0.5, "zero retrigger Attack midpoint");

    program.nodes[14].flags = 0;
    const std::array repress{on(0, 1), off(9600, 1), on(16800, 2, 72)};
    Signals release(program.nodes.size(), 18000);
    render(program, repress, 18000, blocks, &release);
    near(release.values[14][16800], 0.3, "Release retrigger preserves current amplitude");
    near(release.values[14][17040], 0.65, "gate rise plus trigger starts Attack once");
}

void mono_glide_and_state_continuity() {
    auto program = envelope_program(true);
    program.voice_policy.retrigger = Retrigger::gate_rise;
    program.voice_policy.glide_seconds = 0.1;
    const std::array events{on(0, 1), on(24000, 2, 72), on(26400, 3, 74),
                            off(36000, 3), off(45600, 2), off(57600, 1)};
    const std::array<std::size_t, 1> blocks{64};
    Signals signals(program.nodes.size(), 82000);
    render(program, events, 82000, blocks, &signals);
    const auto& pitch = signals.values[0];
    near(pitch[24000], 69, "glide starts at current pitch"); near(pitch[26400], 70.5, "glide retarget midpoint");
    near(pitch[28800], 72.25, "glide restarts from current pitch"); near(pitch[31200], 74, "glide target");
    near(pitch[40800], 72, "glide on fallback"); near(pitch[50400], 69, "glide final fallback");
    near(signals.values[9][24000], 0, "gate_rise suppresses overlapping trigger");
    near(signals.values[14][24000], 0.6, "legato envelope remains in sustain");

    const std::array release{on(0, 1), on(24000, 2, 72), off(24960, 1), off(26400, 2)};
    Signals tail(program.nodes.size(), 53000);
    render(program, release, 53000, blocks, &tail);
    near(tail.values[0][26400], 70.5, "release during glide pitch");
    near(tail.values[0][28800], 72, "release must not cancel glide");
    near(tail.values[14][40800], 0, "release during glide envelope completes");

    // Mono's oscillator runs through idle; note-on does not reset its phase.
    const std::array delayed{on(101, 1)};
    Signals idle(program.nodes.size(), 256);
    render(program, delayed, 256, blocks, &idle);
    near(idle.values[6][101], std::sin(tau * 440.0 * 101 / 48000.0) * std::pow(10.0, -6.0 / 20.0),
         "mono phase restarted at note-on", 1e-7);
}

void dynamic_partition_invariance() {
    auto program = envelope_program(true);
    program.voice_policy.glide_seconds = 0.1;
    const std::array events{on(13, 1), on(501, 2, 74), off(1703, 2), off(2099, 1),
                            on(6101, 3, 60), off(7003, 3)};
    const std::array<std::size_t, 1> singles{1}, blocks{64};
    const std::array<std::size_t, 8> random{13, 127, 3, 256, 91, 1, 64, 217};
    const auto one = render(program, events, 24000, singles);
    check(one == render(program, events, 24000, blocks), "ADSR/glide output depends on block size");
    check(one == render(program, events, 24000, random), "ADSR/glide output depends on random partitions");
}

void poly_release_completion() {
    auto program = envelope_program();
    program.nodes[10].initial_value = 0.0;
    program.nodes[13].initial_value = 0.01;
    const std::array events{on(0, 1), on(100, 2), off(200, 1), off(300, 2), on(680, 3), off(700, 3)};
    const std::array<std::size_t, 1> blocks{64};
    Signals signals(program.nodes.size(), 1300);
    signals.selected_note = 1;
    const auto audio = render(program, events, 1300, blocks, &signals, 2);
    near(signals.values[1][200], 0, "poly note-off changes gate during Release");
    check(std::abs(audio[300]) > 1e-5, "poly release was cut off at note-off");
    check(signals.completions == 3, "poly completion count or exact-sample slot availability is wrong");
    check(signals.completion_times[0] == 680 && signals.completion_times[1] == 780 && signals.completion_times[2] == 1180,
          "poly Release completion sample is wrong");
    check(std::all_of(audio.begin() + 1180, audio.end(), [](float value) { return value == 0; }), "completed poly voice remains audible");
}

void live_parameters_preserve_state() {
    auto program = sine_program();
    ParameterDescriptor descriptor;
    descriptor.value = 3; descriptor.binding_id = "level"; descriptor.smoothing_seconds = 0.01;
    descriptor.has_range = true; descriptor.min = -96; descriptor.max = 0; descriptor.unit = Unit::decibels;
    program.parameters.push_back(descriptor);
    const std::array events{on(0, 1)};
    const std::array updates{ParameterUpdate{137, 3, -12.0}, ParameterUpdate{333, 3, -3.0}};
    const std::array<std::size_t, 1> blocks{64}, singles{1};
    const auto audio = render(program, events, 1200, blocks, nullptr, 32, updates);
    check(audio == render(program, events, 1200, singles, nullptr, 32, updates), "parameter smoothing depends on block size");
    double current = -6, start = -6, target = -6;
    std::size_t elapsed = 0, length = 0;
    for (std::size_t i = 0; i < audio.size(); ++i) {
        for (const auto& update : updates) if (update.sample == i) { start = current; target = update.value; elapsed = 0; length = 480; }
        const auto expected = std::sin(tau * 440.0 * double(i) / 48000.0) * std::pow(10.0, static_cast<float>(current) / 20.0);
        near(audio[i], expected, "live level update reset phase or used wrong smoothing", 1.5e-7);
        if (elapsed < length) { ++elapsed; current = start + (target - start) * double(elapsed) / double(length); }
    }

    auto filtered = sine_program();
    filtered.states.push_back({"filter", {}, StateKind::filter});
    filtered.nodes.push_back(node(Op::parameter, {}, 1)); // DC input 9
    filtered.nodes.push_back(node(Op::parameter, {}, 800)); // cutoff 10
    filtered.nodes.push_back(node(Op::parameter, {}, 0.5)); // resonance 11
    filtered.nodes.push_back(node(Op::filter_lowpass, {9, 10, 11}, 0, 1));
    filtered.output = 12;
    descriptor.value = 10; descriptor.binding_id = "cutoff"; descriptor.min = 5; descriptor.max = 20000;
    descriptor.smoothing_seconds = 0.005; descriptor.logarithmic = true; descriptor.unit = Unit::hertz;
    filtered.parameters.push_back(descriptor);
    const std::array cutoff_updates{ParameterUpdate{12000, 10, 40}};
    const auto stable_dc = render(filtered, events, 15000, blocks, nullptr, 32, cutoff_updates);
    for (std::size_t i = 11900; i < stable_dc.size(); ++i)
        near(stable_dc[i], 1, "cutoff update reset filter state", 1e-6);

    auto envelope = envelope_program();
    descriptor.value = 11; descriptor.binding_id = "D"; descriptor.min = 0; descriptor.max = 10;
    descriptor.smoothing_seconds = 0; descriptor.logarithmic = false; descriptor.unit = Unit::seconds;
    envelope.parameters.push_back(descriptor);
    const std::array decay_at_start{ParameterUpdate{480, 11, 0.2}};
    Signals stages(envelope.nodes.size(), 6000);
    render(envelope, events, 6000, blocks, &stages, 32, decay_at_start);
    near(stages.values[14][5280], 0.8, "Decay must read new duration at actual stage start");
}

void filter_response_and_saw_alias_reduction() {
    auto program = sine_program();
    program.nodes[6].op = Op::saw;
    program.states.push_back({"filter", {}, StateKind::filter});
    program.nodes.push_back(node(Op::parameter, {}, 800)); // 9
    program.nodes.push_back(node(Op::parameter, {}, 0.15)); // 10
    program.nodes.push_back(node(Op::filter_lowpass, {6, 9, 10}, 0, 1)); // 11
    program.nodes.push_back(node(Op::multiply, {11, 1})); // 12
    program.output = 12;
    program.named_values.push_back({"filtered", 11, {}});
    const std::array events{on(0, 1, 57)};
    const std::array<std::size_t, 1> blocks{64}, singles{1};
    Signals signals(program.nodes.size(), 4800);
    const auto audio = render(program, events, 4800, blocks, &signals);
    check(audio == render(program, events, 4800, singles), "filter depends on block partition");
    double raw_delta = 0, filtered_delta = 0;
    for (std::size_t i = 1000; i < audio.size(); ++i) {
        raw_delta += std::pow(signals.values[6][i] - signals.values[6][i-1], 2);
        filtered_delta += std::pow(signals.values[11][i] - signals.values[11][i-1], 2);
    }
    check(filtered_delta < raw_delta * 0.3, "lowpass failed to attenuate saw high frequencies");

    // TPT filter DC reference: LP settles to one, HP rejects it, including high resonance.
    auto dc = sine_program();
    dc.states.push_back({"filter", {}, StateKind::filter});
    dc.nodes.push_back(node(Op::parameter, {}, 1));
    dc.nodes.push_back(node(Op::parameter, {}, 800));
    dc.nodes.push_back(node(Op::parameter, {}, 1));
    dc.nodes.push_back(node(Op::filter_lowpass, {9, 10, 11}, 0, 1));
    dc.output = 12;
    const auto low = render(dc, events, 24000, blocks);
    near(low.back(), 1, "lowpass DC gain", 2e-5);
    dc.nodes[12].op = Op::filter_highpass;
    const auto high = render(dc, events, 24000, blocks);
    near(high.back(), 0, "highpass DC rejection", 2e-5);

    // Independent DFT of a folded sixth harmonic: naive saw aliases it to bin 1696.
    auto saw = sine_program();
    saw.nodes[4] = node(Op::parameter, {}, 4687.5);
    saw.nodes[6].op = Op::saw;
    saw.output = 6;
    const auto bandlimited = render(saw, events, 4096, blocks);
    double naive_real = 0, naive_imag = 0, corrected_real = 0, corrected_imag = 0;
    for (std::size_t i = 0; i < bandlimited.size(); ++i) {
        const auto phase = std::fmod(double(i) * 400.0 / 4096.0, 1.0);
        const auto naive = (2.0 * phase - 1.0) * std::pow(10.0, -6.0 / 20.0);
        const auto angle = tau * 1696.0 * double(i) / 4096.0;
        naive_real += naive * std::cos(angle); naive_imag += naive * std::sin(angle);
        corrected_real += bandlimited[i] * std::cos(angle); corrected_imag += bandlimited[i] * std::sin(angle);
    }
    check(std::hypot(corrected_real, corrected_imag) < std::hypot(naive_real, naive_imag) * 0.5,
          "polyBLEP did not reduce the measured folded harmonic");
}

void filter_tail_completion() {
    auto program = envelope_program();
    program.nodes[10].initial_value = 0;
    program.nodes[13].initial_value = 0.01;
    program.states.push_back({"tail_filter", {}, StateKind::filter});
    program.nodes.push_back(node(Op::parameter, {}, 800)); // 17
    program.nodes.push_back(node(Op::parameter, {}, 0)); // 18
    program.nodes.push_back(node(Op::filter_lowpass, {16, 17, 18}, 0, 2)); // 19
    program.output = 19;
    program.completion.push_back({CompletionOp::filter_settled, 2, {}});
    program.completion.push_back({CompletionOp::all, 0, {0, 1}});
    program.completion_root = 2;
    const std::array events{on(0, 1), off(500, 1)};
    const std::array<std::size_t, 1> blocks{64}, singles{1};
    Signals signals(program.nodes.size(), 3000);
    const auto audio = render(program, events, 3000, blocks, &signals);
    check(signals.completions == 1 && signals.completion_times[0] > 980 && signals.completion_times[0] < 3000,
          "filter tail did not keep the poly voice until state settled");
    check(std::abs(audio[980]) > 1e-7, "filter tail was cut off when envelope ended");
    check(audio == render(program, events, 3000, singles), "filter tail completion depends on block size");
}

void automation_ramp_endpoints() {
    auto program = sine_program();
    program.output = 3;
    ParameterDescriptor descriptor;
    descriptor.value = 3; descriptor.binding_id = "level"; descriptor.has_range = true;
    descriptor.min = -96; descriptor.max = 0; descriptor.smoothing_seconds = 0.005;
    program.parameters.push_back(descriptor);
    const std::array events{on(0, 1)};
    const std::array updates{ParameterUpdate{13, 3, -16, 100, false}, ParameterUpdate{150, 3, -3, 0, false}};
    const std::array<std::size_t, 1> blocks{64}, singles{1};
    const auto audio = render(program, events, 256, blocks, nullptr, 32, updates);
    check(audio == render(program, events, 256, singles, nullptr, 32, updates), "explicit ramp depends on block partition");
    near(audio[13], -6, "ramp start value"); near(audio[63], -11, "ramp linear midpoint");
    near(audio[113], -16, "ramp endpoint sample"); near(audio[150], -3, "automation step incorrectly used authored smoothing");

    program.nodes[3].initial_value = 100;
    program.nodes[3].unit = Unit::hertz;
    program.nodes[5] = node(Op::parameter, {}, 1);
    program.parameters[0].min = 20; program.parameters[0].max = 20000;
    program.parameters[0].unit = Unit::hertz;
    program.parameters[0].logarithmic = true;
    const std::array linear{ParameterUpdate{13, 3, 400, 100, false}};
    const std::array logarithmic{ParameterUpdate{13, 3, 400, 100, true}};
    const auto linear_audio = render(program, events, 256, blocks, nullptr, 32, linear);
    const auto log_audio = render(program, events, 256, blocks, nullptr, 32, logarithmic);
    near(linear_audio[63], 250, "canonical linear automation inherited a logarithmic UI scale");
    near(log_audio[63], 200, "log-mapped automation midpoint"); near(log_audio[113], 400, "log ramp endpoint");
    const std::array explicit_start{ParameterUpdate{13, 3, 400, 100, false, 200.0}};
    const auto overridden = render(program, events, 256, blocks, nullptr, 32, explicit_start);
    near(overridden[13], 200, "host ramp explicit start"); near(overridden[63], 300, "host ramp start/end interpolation");
    near(overridden[113], 400, "host ramp start/end endpoint");
    Engine validation(program, {48000, 64, 32});
    std::array<float, 64> silence{};
    const std::array invalid_start{ParameterUpdate{0, 3, 400, 10, false, -1.0}};
    check(validation.process({}, silence, nullptr, invalid_start).error == ProcessError::invalid_parameter,
          "out-of-range ramp start was accepted");
    const std::array implicit_duration{ParameterUpdate{64, 3, 400, std::nullopt, false, 200.0}};
    check(validation.process({}, silence, nullptr, implicit_duration).error == ProcessError::invalid_parameter,
          "ramp_start without explicit duration was accepted");
}

void restore_and_realtime_errors() {
    auto program = envelope_program(true);
    ParameterDescriptor sustain;
    sustain.value = 12; sustain.binding_id = "S"; sustain.min = 0; sustain.max = 1;
    sustain.has_range = true; sustain.smoothing_seconds = 0.005;
    program.parameters.push_back(sustain);
    Engine engine(program, {48000, 64, 32});
    std::array<Event, 1024> notes{};
    for (std::size_t i = 0; i < notes.size(); ++i) notes[i] = on(0, i, 60 + i % 12);
    count_heap = true;
    const auto restored = engine.restore_held_notes(notes);
    count_heap = false;
    check(static_cast<bool>(restored) && engine.stats().processed_samples == 0 && engine.stats().accepted_notes == 1024,
          "held ledger replay failed or advanced time");
    std::array<float, 64> audio{};
    const std::array updates{ParameterUpdate{4, 12, 0.3}, ParameterUpdate{12, 12, 0.7}};
    count_heap = true;
    const auto rendered = engine.process({}, audio, nullptr, updates);
    engine.panic();
    const std::array invalid{off(64, 9999)};
    const auto failed = engine.process(invalid, audio);
    count_heap = false;
    check(static_cast<bool>(rendered) && failed.error == ProcessError::unknown_note &&
          engine.stats().active_voices == 0 && engine.stats().processed_samples == 128,
          "RT panic/error behavior is wrong");
    check(std::all_of(audio.begin(), audio.end(), [](float sample) { return sample == 0; }), "failed callback is not silent");
    check(allocation_count == 0 && deallocation_count == 0, "RT restore/panic/error allocated or freed memory");
}


struct StereoAudio { std::vector<float> left, right; };
StereoAudio render_stereo(const Program& program, std::span<const Event> events, std::size_t frames,
                          std::span<const std::size_t> blocks, CaptureSink* captures = nullptr) {
    Engine engine(program, {48000, 256, 32});
    StereoAudio result{std::vector<float>(frames), std::vector<float>(frames)};
    std::size_t cursor = 0, event_cursor = 0, partition = 0;
    while (cursor < frames) {
        const auto count = std::min(blocks[partition++ % blocks.size()], frames - cursor);
        auto end = event_cursor;
        while (end < events.size() && events[end].sample < cursor + count) ++end;
        check(static_cast<bool>(engine.process_stereo(events.subspan(event_cursor, end - event_cursor),
            std::span<float>(result.left).subspan(cursor, count), std::span<float>(result.right).subspan(cursor, count), captures)), "stereo render failed");
        cursor += count; event_cursor = end;
    }
    return result;
}

ValueId append(Program& program, Node value, std::uint32_t channels = 1) {
    value.channels = channels;
    program.nodes.push_back(std::move(value));
    return static_cast<ValueId>(program.nodes.size() - 1);
}

void stereo_arithmetic_filter_and_capture() {
    auto program = sine_program();
    const auto zero = append(program, node(Op::parameter, {}, 0));
    const auto negative = append(program, node(Op::subtract, {zero, program.output}));
    const auto joined = append(program, node(Op::stereo, {program.output, negative}), 2);
    const auto cutoff = append(program, node(Op::parameter, {}, 800));
    const auto resonance = append(program, node(Op::parameter, {}, 0.3));
    program.states.push_back({"stereo_filter", {}, StateKind::filter});
    program.output = append(program, node(Op::filter_lowpass, {joined, cutoff, resonance}, 0, 1), 2);
    const auto half = append(program, node(Op::parameter, {}, .5));
    const auto scaled = append(program, node(Op::multiply, {program.output, half}), 2);
    program.output = append(program, node(Op::add, {scaled, scaled}), 2);
    program.named_values.push_back({"stereo", program.output, {}});
    const auto stereo_output = program.output;
    const std::array events{on(0, 1)};
    const std::array<std::size_t, 1> singles{1}, blocks{127};
    Signals downmixed(program.nodes.size(), 4096);
    const auto audio = render_stereo(program, events, 4096, blocks, &downmixed);
    const auto partitioned = render_stereo(program, events, 4096, singles);
    check(audio.left == partitioned.left && audio.right == partitioned.right, "stereo filter depends on block partition");
    const auto mono = render(program, events, 4096, blocks);
    for (std::size_t i = 0; i < audio.left.size(); ++i) {
        near(audio.left[i], -audio.right[i], "stereo filter leaked across channel states", 1e-7);
        check(mono[i] == 0 && downmixed.values[stereo_output][i] == 0, "legacy output/capture must be exact stereo Mid");
    }
    for (auto op : {Op::left, Op::right, Op::mid, Op::side}) {
        program.output = append(program, node(op, {stereo_output}));
        const auto projected = render(program, events, 4096, blocks);
        for (std::size_t i = 0; i < projected.size(); ++i)
            near(projected[i], op == Op::right ? audio.right[i] : op == Op::mid ? 0.0 : audio.left[i], "stereo channel projection incorrect", 1e-7);
    }
    struct StereoCapture final : CaptureSink {
        ValueId selected; std::size_t count = 0; double difference = 0;
        explicit StereoCapture(ValueId value) : selected(value) {}
        void capture(ValueId, VoiceIdentity, std::uint64_t, std::span<const float>) noexcept override {}
        void event(const Event&, VoiceIdentity, std::string_view) noexcept override {}
        void capture_stereo(ValueId value, VoiceIdentity, std::uint64_t, std::span<const float> left, std::span<const float> right) noexcept override {
            if (value != selected) return;
            count += left.size();
            for (std::size_t i = 0; i < left.size(); ++i) difference += std::abs(left[i] - right[i]);
        }
    } capture(stereo_output);
    render_stereo(program, events, 4096, blocks, &capture);
    check(capture.count == 4096 && capture.difference > 100, "stereo captures lost a channel");
    Engine engine(program, {48000, 64, 32});
    std::array<float, 64> left{}, right{};
    check(engine.process_stereo({}, left, std::span<float>(right).first(63)).error == ProcessError::block_capacity,
          "stereo buffer length mismatch must be rejected");
    auto invalid = program; invalid.nodes[joined].channels = 1;
    rejects([&]{Engine bad(invalid, {48000, 64, 32});}, "invalid stereo topology accepted");
}

Program unison_program(std::uint32_t count = 2) {
    Program program; program.states.push_back({"wide", {}, StateKind::oscillator});
    for (double value : {440.0, 0.5, 10.0, 1.0, 0.0, 0.0, 0.0}) append(program, node(Op::parameter, {}, value));
    auto oscillator = node(Op::unison_sine, {0, 1, 2, 3, 4, 5, 6}); oscillator.flags = count;
    const auto raw = append(program, oscillator, 2);
    const auto gate = append(program, node(Op::key_gate));
    program.output = append(program, node(Op::multiply, {raw, gate}), 2);
    program.named_values = {{"raw", raw, {}}};
    return program;
}

double measured_frequency(const std::vector<float>& signal) {
    double first = 0, last = 0; std::size_t crossings = 0;
    for (std::size_t i = 1; i < signal.size(); ++i) if (signal[i-1] <= 0 && signal[i] > 0) {
        const auto crossing = double(i-1) - signal[i-1] / double(signal[i] - signal[i-1]);
        if (crossings++ == 0) first = crossing;
        last = crossing;
    }
    return 48000.0 * (crossings - 1) / (last - first);
}

void unison_tuning_pan_phase_and_random() {
    auto program = unison_program();
    const std::array events{on(0, 10)};
    const std::array<std::size_t, 1> singles{1}, blocks{127};
    const auto audio = render_stereo(program, events, 24000, blocks);
    const auto partitioned = render_stereo(program, events, 24000, singles);
    check(audio.left == partitioned.left && audio.right == partitioned.right, "unison partitioning changed phase or random seed");
    check(std::abs(measured_frequency(audio.left) - 440.0 * std::exp2(-10.0/1200.0)) < .01 &&
          std::abs(measured_frequency(audio.right) - 440.0 * std::exp2(10.0/1200.0)) < .01,
          "unison detune is not symmetric in cents");
    program = unison_program(8); program.nodes[2].initial_value = 0; program.nodes[3].initial_value = 0;
    const auto centered = render_stereo(program, events, 1024, blocks);
    for (std::size_t i = 0; i < centered.left.size(); ++i) {
        near(centered.left[i], .5 * std::sin(tau * 440 * i / 48000), "coherent unison was not normalized", 1e-7);
        near(centered.left[i], centered.right[i], "zero spread does not produce mono", 1e-7);
    }
    program = unison_program(1); program.nodes[3].initial_value = 0; program.nodes[4].initial_value = -1;
    program.nodes[5].initial_value = .25;
    const auto panned = render_stereo(program, events, 512, blocks);
    near(panned.left.front(), .5 * std::sqrt(2.0), "authored initial phase not applied");
    check(std::all_of(panned.right.begin(), panned.right.end(), [](float sample){return std::abs(sample) < 1e-7;}), "hard-left pan leaked right");
    program = unison_program(8); program.nodes[6].initial_value = 1;
    const auto random = render_stereo(program, events, 1024, blocks);
    const auto repeated = render_stereo(program, events, 1024, singles);
    check(random.left == repeated.left && random.right == repeated.right, "random phase is not deterministic");
    const std::array other_note{on(0, 11)};
    check(random.left != render_stereo(program, other_note, 1024, blocks).left, "different note identities reused random phase");
    program.voice_policy.mode = VoiceMode::mono;
    const std::array fallback{on(0, 10), on(64, 11), off(128, 11)};
    const std::array kept{on(0, 10), on(64, 11)};
    check(render_stereo(program, fallback, 256, blocks).left == render_stereo(program, kept, 256, blocks).left,
          "mono priority fallback restarted oscillator phases");
    program.voice_policy.retrigger = Retrigger::gate_rise;
    const std::array legato{on(0, 10), on(64, 11)};
    check(render_stereo(program, legato, 256, blocks).left == render_stereo(program, events, 256, blocks).left,
          "mono legato restarted unison phase despite gate_rise policy");
}

Program lfo_program(std::uint32_t shape, std::uint32_t mode, bool bipolar = true, bool sync = false) {
    Program program; program.voice_policy.mode = VoiceMode::mono;
    program.states.push_back({"modulator", {}, StateKind::lfo});
    program.nodes = {node(Op::parameter, {}, sync ? .25 : 100.0), node(Op::key_trigger),
                     node(Op::parameter, {}, 0), node(Op::parameter, {}, 0), node(Op::parameter, {}, 0)};
    auto lfo = node(Op::lfo, {0, 1, 2, 3, 4}); lfo.flags = shape | (mode << 3) | (bipolar ? 32 : 0) | (sync ? 64 : 0);
    program.output = append(program, lfo); program.named_values = {{"lfo", program.output, {}}};
    return program;
}

void lfo_shapes_modes_and_transport() {
    const std::array events{on(0, 1), on(120, 2)};
    const std::array<std::size_t, 1> singles{1}, blocks{127};
    for (std::uint32_t shape = 0; shape < 5; ++shape) {
        auto program = lfo_program(shape, 1);
        const auto audio = render(program, events, 1500, blocks);
        check(audio == render(program, events, 1500, singles), "LFO depends on block partition");
        if (shape != 4) near(audio[0], audio[120], "retrigger did not reset LFO phase");
        check(std::all_of(audio.begin(), audio.end(), [](float sample){return std::isfinite(sample) && std::abs(sample)<=1.000001f;}), "LFO range is invalid");
        if (shape == 4) check(audio[121] == audio[590] && audio[121] != audio[610], "sample-and-hold did not hold one value per cycle");
    }
    auto delayed = lfo_program(0, 1); delayed.nodes[0].initial_value = 1; delayed.nodes[2].initial_value = .25;
    delayed.nodes[3].initial_value = .01; delayed.nodes[4].initial_value = .01;
    const std::array start{on(0, 1)};
    const auto faded = render(delayed, start, 1100, blocks);
    check(std::all_of(faded.begin(), faded.begin()+481, [](float sample){return sample==0;}), "LFO delay/fade boundary incorrect");
    near(faded[720], .5 * std::sin(tau * (.25 + 240.0 / 48000)), "LFO fade midpoint incorrect");
    auto once = lfo_program(2, 2);
    const auto one_shot = render(once, start, 1200, blocks);
    near(one_shot[0], -1, "one-shot initial phase");
    near(one_shot[600], 1, "one-shot did not hold endpoint"); near(one_shot[1199], 1, "one-shot restarted by itself");
    auto free = lfo_program(2, 0);
    const auto running = render(free, events, 500, blocks);
    near(running[120], -.5, "free LFO restarted on a key trigger");
    auto synced = lfo_program(0, 3, true, true);
    Engine engine(synced, {48000, 256, 32}); std::array<float, 256> output{};
    engine.set_transport({120, .25, true, true});
    check(static_cast<bool>(engine.process({}, output)), "transport render failed");
    near(output[0], 1, "transport PPQ phase incorrect");
    engine.set_transport({120, .25, false, true}); engine.process({}, output);
    check(std::all_of(output.begin(), output.end(), [](float sample){return sample==1;}), "stopped transport LFO did not freeze");
    engine.set_transport({60, 0, true, true}); engine.process({}, output);
    near(output[240], std::sin(tau * .005), "tempo-synced quarter-note LFO ignored BPM");
    auto mono_reference = lfo_program(4, 3, true, true);
    auto poly_late = mono_reference; poly_late.voice_policy.mode = VoiceMode::poly;
    const std::array late{on(480, 1)};
    const auto reference = render(mono_reference, {}, 1200, blocks);
    const auto late_audio = render(poly_late, late, 1200, blocks);
    check(std::equal(reference.begin()+480, reference.end(), late_audio.begin()+480), "transport sample-and-hold differs across voice start times");
}

Program distortion_program(std::uint32_t mode) {
    auto program = sine_program(); program.nodes[3].initial_value = 0;
    program.states.push_back({"drive", {}, StateKind::distortion});
    const auto drive = append(program, node(Op::parameter, {}, 4));
    const auto mix = append(program, node(Op::parameter, {}, 1));
    const auto output = append(program, node(Op::parameter, {}, 1));
    auto dist = node(Op::distortion, {program.output, drive, mix, output}, 0, 1); dist.flags = mode;
    program.output = append(program, dist);
    program.completion = {{CompletionOp::gate_closed, 0, {}}, {CompletionOp::distortion_settled, 1, {}}, {CompletionOp::all, 0, {0,1}}};
    program.completion_root = 2;
    return program;
}

double dft_magnitude(const std::vector<float>& audio, std::size_t bin) {
    double real = 0, imaginary = 0;
    for (std::size_t i = 0; i < audio.size(); ++i) {
        const auto angle = tau * bin * i / audio.size();
        real += audio[i] * std::cos(angle); imaginary += audio[i] * std::sin(angle);
    }
    return std::hypot(real, imaginary);
}

void distortion_antialiasing_bypass_and_tail() {
    const std::array start{on(0, 1)};
    const std::array<std::size_t, 1> singles{1}, blocks{127};
    for (std::uint32_t mode=0; mode<3; ++mode) {
        auto program = distortion_program(mode);
        const auto audio = render(program, start, 4096, blocks);
        check(audio == render(program, start, 4096, singles), "distortion depends on block partition");
        check(std::all_of(audio.begin(), audio.end(), [](float sample){return std::isfinite(sample) && std::abs(sample)<=1.000001f;}), "distortion produced a nonfinite/unbounded output");
        program.nodes[10].initial_value = 0;
        auto bypass = sine_program(); bypass.nodes[3].initial_value = 0;
        check(render(program, start, 4096, blocks) == render(bypass, start, 4096, blocks), "mix:0 must be an exact dry bypass");
    }
    auto hard = distortion_program(1); hard.nodes[4] = node(Op::parameter, {}, 4687.5);
    const auto antialiased = render(hard, start, 4096, blocks);
    std::vector<float> naive(4096);
    for (std::size_t i=0;i<naive.size();++i) naive[i]=static_cast<float>(std::clamp(4.0*std::sin(tau*400*i/4096.0),-1.0,1.0));
    const auto ratio = dft_magnitude(antialiased,1296)/dft_magnitude(antialiased,400);
    const auto naive_ratio = dft_magnitude(naive,1296)/dft_magnitude(naive,400);
    check(ratio < naive_ratio*.65, "ADAA failed to reduce folded seventh harmonic relative to the fundamental");
    auto tail = distortion_program(0); const std::array notes{on(0,1),off(5,1)};
    Signals capture(tail.nodes.size(),32); const auto audio=render(tail,notes,32,blocks,&capture);
    check(std::abs(audio[5])>1e-5 && audio[6]==0 && capture.completions==1 && capture.completion_times[0]==6,
          "distortion's final interval was cut off or voice never completed");
    check(audio == render(tail,notes,32,singles), "distortion completion depends on block size");

    auto stereo = sine_program();
    const auto zero = append(stereo,node(Op::parameter,{},0));
    const auto negative = append(stereo,node(Op::subtract,{zero,stereo.output}));
    const auto pair = append(stereo,node(Op::stereo,{stereo.output,negative}),2);
    const auto drive = append(stereo,node(Op::parameter,{},4));
    const auto unity = append(stereo,node(Op::parameter,{},1));
    stereo.states.push_back({"stereo_drive",{},StateKind::distortion});
    stereo.output = append(stereo,node(Op::distortion,{pair,drive,unity,unity},0,1),2);
    for (std::uint32_t mode=0;mode<3;++mode) {
        stereo.nodes[stereo.output].flags=mode;
        const auto channels=render_stereo(stereo,start,1024,blocks);
        for(std::size_t i=0;i<channels.left.size();++i)
            near(channels.left[i],-channels.right[i],"stereo distortion shared channel history",1e-7);
    }
}

void stereo_effects_realtime_heap() {
    auto program=unison_program(8);
    program.states.push_back({"drive",{},StateKind::distortion});
    const auto drive=append(program,node(Op::parameter,{},2)),mix=append(program,node(Op::parameter,{},.5)),gain=append(program,node(Op::parameter,{},.5));
    program.output=append(program,node(Op::distortion,{program.output,drive,mix,gain},0,1),2);
    program.states.push_back({"motion",{},StateKind::lfo});
    const auto rate=append(program,node(Op::parameter,{},2)),trigger=append(program,node(Op::key_trigger)),zero=append(program,node(Op::parameter,{},0));
    const auto lfo=append(program,node(Op::lfo,{rate,trigger,zero,zero,zero},0,2));
    program.output=append(program,node(Op::multiply,{program.output,lfo}),2);
    program.named_values.push_back({"motion",lfo,{}});
    Engine engine(program,{48000,256,32});
    std::array<Event,32> notes{};for(std::size_t i=0;i<notes.size();++i)notes[i]=on(0,i+1,48+i);
    std::array<float,256> left{},right{}; FixedCapture capture;
    count_heap=true;
    const auto result=engine.process_stereo(notes,left,right,&capture);
    engine.set_transport({138,2,true,true}); engine.panic();
    count_heap=false;
    check(static_cast<bool>(result) && allocation_count==0 && deallocation_count==0,"stereo unison/distortion/capture allocated on the audio thread");
}

} // namespace

int main() {
    try {
        analytic_reference();
        partition_invariance();
        note_identity_and_phase();
        independent_oscillator_state();
        capture_is_observational();
        bounded_voice_admission();
        validation_is_atomic();
        held_ledger_capacity();
        no_heap_in_valid_process();
        envelope_timing_and_retrigger();
        mono_glide_and_state_continuity();
        dynamic_partition_invariance();
        poly_release_completion();
        live_parameters_preserve_state();
        filter_response_and_saw_alias_reduction();
        filter_tail_completion();
        automation_ramp_endpoints();
        restore_and_realtime_errors();
        stereo_arithmetic_filter_and_capture();
        unison_tuning_pan_phase_and_random();
        lfo_shapes_modes_and_transport();
        distortion_antialiasing_bypass_and_tail();
        stereo_effects_realtime_heap();
        std::cout << "Engine: 23 test groups passed (stereo, unison, LFO/transport, distortion ADAA, legacy DSP, captures, RT bounds/heap).\n";
        return 0;
    } catch (const std::exception& error) {
        count_heap = false;
        std::cerr << "Engine test failure: " << error.what() << '\n';
        return 1;
    }
}
