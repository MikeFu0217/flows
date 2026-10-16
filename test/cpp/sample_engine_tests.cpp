#include "engine/engine.hpp"

#include <array>
#include <cmath>
#include <chrono>
#include <vector>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

namespace { bool watch_heap = false; std::size_t heap_calls = 0; }
void* operator new(std::size_t n) { if (watch_heap) ++heap_calls; if (auto* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { if (watch_heap && p) ++heap_calls; std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

namespace {
using namespace flows;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void near(float actual, double expected, const char* message) {
    if (std::abs(double(actual) - expected) > 1e-6) { std::cerr << actual << " != " << expected << '\n'; check(false, message); }
}
template <class F> void rejects(F f) { bool threw = false; try { f(); } catch (const std::invalid_argument&) { threw = true; } check(threw, "invalid resource accepted"); }
Node constant(double value) { Node n; n.initial_value = value; return n; }
Program instrument(bool loop = false, bool reverse = false) {
    Program p;
    p.samples.push_back({"Samples/test.wav", {}});
    p.states.push_back({"sample", {}, StateKind::sampler});
    p.nodes = {constant(0), constant(0), constant(48), constant(1), constant(0), constant(1), constant(1), constant(0), {}};
    p.nodes[0].op = Op::key_trigger; p.nodes[1].op = Op::key_pitch; p.nodes[7].op = Op::key_gate;
    auto& sample = p.nodes[8]; sample.op = Op::sampler; sample.channels = 2; sample.flags = (loop ? 1U : 0U) | (reverse ? 2U : 0U);
    sample.inputs = {0, 1, 2, 3, 4, 5, 6, 7}; p.output = 8;
    p.completion = {{loop ? CompletionOp::gate_closed : CompletionOp::sample_done, 0, {}}}; p.completion_root = 0;
    return p;
}
SampleBank bank(double rate = 48000, bool stereo = false) {
    auto s = std::make_shared<SampleData>(); s->sample_rate = rate; s->left = {0.125f, 0.25f, 0.5f, 1.0f};
    if (stereo) s->right = {-0.25f, -0.5f, -0.75f, -1.0f};
    return {s};
}
Event on(std::uint64_t at = 0, std::uint64_t id = 1, double pitch = 48) { return {at, EventType::note_on, id, pitch, 1}; }
Event off(std::uint64_t at, std::uint64_t id = 1) { return {at, EventType::note_off, id}; }
void basics() {
    auto p = instrument(); Engine engine(p, {}, bank(48000, true)); std::array<float, 8> l{}, r{};
    const std::array events{on(), off(1)};
    check(bool(engine.process_stereo(events, l, r)), "stereo process failed");
    const std::array<float, 8> left{0.125f, 0.25f, 0.5f, 1, 0, 0, 0, 0}, right{-0.25f, -0.5f, -0.75f, -1, 0, 0, 0, 0};
    for (unsigned i = 0; i < 8; ++i) { near(l[i], left[i], "one-shot left or note-off behavior"); near(r[i], right[i], "stereo right channel"); }
    check(engine.stats().completed_notes == 1 && engine.stats().active_voices == 0, "EOF did not release voice");
    Engine mono(p, {}, bank()); check(bool(mono.process_stereo(std::array{on()}, l, r)), "mono sample process");
    check(l == r, "mono sample must duplicate channels");
    Engine held(p, {}, bank()); check(bool(held.process(std::array{on()}, l)), "held sample process");
    check(held.stats().completed_notes == 1, "held key prevented one-shot completion");
}
void rate_pitch_and_regions() {
    auto p = instrument(); p.nodes[3].initial_value = 0.5;
    Engine half(p, {}, bank()); std::array<float, 10> out{};
    check(bool(half.process(std::array{on()}, out)), "half rate process");
    const std::array<float, 10> expected{.125f, .1875f, .25f, .375f, .5f, .75f, 1, 1, 0, 0};
    for (unsigned i = 0; i < 4; ++i) near(out[i * 2], expected[i * 2], "resampling must preserve original frames at integer positions");
    const auto half_audio = out;
    near(out[8], 0, "half-rate EOF");
    Engine octave(instrument(), {}, bank()); check(bool(octave.process(std::array{on(0,1,60)}, out)), "octave process");
    check(out[0] != 0 && out[1] != 0, "pitched sample lost its content"); near(out[2], 0, "octave must halve duration");
    Engine converted(instrument(), {}, bank(24000)); check(bool(converted.process(std::array{on()}, out)), "sample-rate conversion");
    for (unsigned i = 0; i < out.size(); ++i) near(out[i], half_audio[i], "source sample rate ignored");
    p = instrument(false, true); p.nodes[4].initial_value = .25; p.nodes[5].initial_value = .75;
    Engine reverse(p, {}, bank()); check(bool(reverse.process(std::array{on()}, out)), "reverse region process");
    near(out[0], .5, "reverse region end"); near(out[1], .25, "reverse region start"); near(out[2], 0, "reverse EOF");
    p.nodes[4].initial_value = .9; p.nodes[5].initial_value = .1;
    Engine empty(p, {}, bank()); check(bool(empty.process(std::array{on()}, out)), "empty region process");
    for (auto v : out) near(v, 0, "invalid dynamic region must be silent");
    check(empty.stats().completed_notes == 1, "invalid region leaked voice");
}
void loop_and_trigger() {
    auto p = instrument(true, true);
    Engine engine(p, {}, bank()); std::array<float, 9> out{};
    check(bool(engine.process(std::array{on(), off(7)}, out)), "reverse loop process");
    const std::array<float,4> backward{1,.5,.25,.125};
    for (unsigned i = 0; i < 7; ++i) near(out[i], backward[i % 4], "reverse loop wrap");
    near(out[7], 0, "loop gate stops exactly"); check(engine.stats().active_voices == 0, "loop gate did not complete");
    p = instrument(); p.voice_policy.mode = VoiceMode::mono; p.voice_policy.retrigger = Retrigger::each_press;
    Engine retrigger(p, {}, bank()); check(bool(retrigger.process(std::array{on(), on(2,2)}, out)), "mono retrigger");
    near(out[2], .125, "mono trigger must restart sample"); near(out[5], 1, "mono retrigger duration");
    p.voice_policy.retrigger = Retrigger::gate_rise;
    Engine legato(p, {}, bank()); check(bool(legato.process(std::array{on(), on(2,2)}, out)), "mono legato");
    near(out[2], .5, "legato must preserve cursor"); near(out[4], 0, "legato natural EOF");
    p = instrument(); p.nodes[0] = constant(0);
    Engine delayed(p, {}, bank()); check(bool(delayed.process(std::array{on()}, out)), "delayed trigger");
    check(delayed.stats().active_voices == 1, "untriggered sampler completed prematurely");
    check(bool(delayed.process(std::array{off(9)}, out)), "untriggered release");
    check(delayed.stats().active_voices == 0, "untriggered sample leaked after key release");
}
void poly_and_reuse() {
    auto p = instrument(); Engine engine(p, {48000, 16, 2}, bank()); std::array<float, 8> out{};
    check(bool(engine.process(std::array{on(), on(2,2)}, out)), "poly process");
    near(out[2], .625, "poly voices do not have independent cursors"); near(out[3], 1.25, "poly sum");
    near(out[4], .5, "second voice survives first EOF"); check(engine.stats().completed_notes == 2, "poly completion");
    p.nodes.push_back({}); p.nodes[9].op = Op::multiply; p.nodes[9].channels = 2; p.nodes[9].inputs = {8,7}; p.output=9;
    p.completion.push_back({CompletionOp::gate_closed, 0, {}}); p.completion.push_back({CompletionOp::any, 0, {0,1}}); p.completion_root = 2;
    Engine reused(p, {48000, 16, 1}, bank());
    check(bool(reused.process(std::array{on(), on(4,2), off(6,1)}, out)), "held ledger reuse");
    near(out[6], .5, "old note-off killed reused voice"); check(reused.stats().accepted_notes == 2, "completed slot not reused");
}
void release_tail_and_heap() {
    auto p = instrument(true); p.nodes[7] = constant(1); // Ungated loop with an external envelope.
    p.states.push_back({"amp", {}, StateKind::envelope});
    p.nodes.push_back({}); p.nodes[9].op = Op::key_gate;
    p.nodes.push_back(constant(0)); p.nodes.push_back(constant(0)); p.nodes.push_back(constant(1)); p.nodes.push_back(constant(4.0/48000));
    p.nodes.push_back({}); p.nodes[14].op = Op::adsr; p.nodes[14].state_index=1; p.nodes[14].inputs={9,0,10,11,12,13};
    p.nodes.push_back({}); p.nodes[15].op=Op::multiply; p.nodes[15].channels=2; p.nodes[15].inputs={8,14}; p.output=15;
    p.completion={{CompletionOp::envelope_idle,1,{}}}; p.completion_root=0;
    Engine engine(p, {48000,16,2}, bank()); std::array<float, 16> out{}; const std::array events{on(),off(4)};
    heap_calls=0; watch_heap=true; const auto result=engine.process(events,out); watch_heap=false;
    check(bool(result), "ADSR loop process"); check(heap_calls==0, "sample audio callback touched heap");
    near(out[4], .125, "loop must continue during external envelope release");
    near(out[7], .25, "release tail truncated"); near(out[8], 0, "release completion");
    check(engine.stats().active_voices==0, "release voice not retired");
}
SampleBank sine_bank(double normalized_frequency, std::size_t frames = 4096) {
    auto sample = std::make_shared<SampleData>(); sample->left.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) sample->left[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * normalized_frequency * i));
    return {sample};
}
void resampling_quality_and_partition() {
    constexpr double tau = 6.283185307179586476925286766559;
    auto p = instrument(true); p.nodes[3].initial_value = 1.5;
    auto low = sine_bank(1.0 / 64.0);
    Engine engine(p, {48000,256,32}, low);
    std::array<float,256> output{};
    check(bool(engine.process(std::array{on()}, output)), "sinc passband render");
    double error = 0;
    for (std::size_t i = 0; i < output.size(); ++i) error = std::max(error,std::abs(double(output[i])-std::sin(tau * 1.5 * i / 64.0)));
    check(error < 0.002, "resampler passband distortion");
    auto high = sine_bank(3.0 / 8.0); p.nodes[3].initial_value = 2;
    Engine rejected(p, {48000,256,32}, high);
    check(bool(rejected.process(std::array{on()}, output)), "sinc stopband render");
    double filtered = 0, naive = 0;
    for (std::size_t i = 0; i < output.size(); ++i) { filtered += double(output[i])*output[i]; const auto v=std::sin(tau * .75 * i); naive += v*v; }
    check(filtered / naive < 0.0001, "sinc must suppress octave-up aliasing by at least 40 dB");
    p.nodes[3].initial_value=1.317; Engine whole(p,{48000,256,32},low), chunks(p,{48000,256,32},low);
    std::array<float,256> reference{}, split{};
    check(bool(whole.process(std::array{on()},reference)),"whole block render");
    check(bool(chunks.process(std::array{on()},std::span(split).first(17))),"partition first");
    check(bool(chunks.process({},std::span(split).subspan(17,61))),"partition second");
    check(bool(chunks.process({},std::span(split).subspan(78))),"partition third");
    check(reference==split,"sample output depends on block partition");
    Engine stress(p,{48000,256,32},low); std::array<Event,32> events{};
    for (unsigned i=0;i<32;++i) events[i]=on(0,i+1,48+i%12);
    check(bool(stress.process(events,output)),"32 voice admission");
    const auto begin=std::chrono::steady_clock::now();
    heap_calls=0; watch_heap=true;
    bool ok=true; for(unsigned i=0;i<100;++i) ok=bool(stress.process({},output)) && ok;
    watch_heap=false;
    const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
    check(ok && heap_calls==0,"32 voice resampling touched heap or failed");
    std::cout << "32-voice resampling: " << elapsed << " s for " << (25600.0/48000) << " s audio; passband error " << error
              << ", octave alias attenuation " << 10.0*std::log10(filtered/naive) << " dB\n";
}

void tiny_regions_and_extreme_controls() {
    for (const double output_rate : {100.0,768000.0}) for (const double source_rate : {100.0,768000.0})
    for (const double pitch : {0.0,127.0}) for (const double root : {0.0,127.0})
    for (const double rate : {1.0/64,64.0}) for (const bool reverse : {false,true}) {
        auto p=instrument(true,reverse); p.nodes[2].initial_value=root; p.nodes[3].initial_value=rate;
        p.nodes[4].initial_value=.501; p.nodes[5].initial_value=.502; // Exactly one selected source frame.
        Engine engine(p,{output_rate,256,1},bank(source_rate)); std::array<float,256> out{};
        check(bool(engine.process(std::array{on(0,1,pitch)},out)),"extreme one-frame region failed");
        for (float value : out) near(value,.5,"one-frame region must preserve DC while wrapping");
    }
    auto p=instrument(); p.nodes[3].op=Op::divide; p.nodes[3].inputs={0,0};
    Engine bad_signal(p,{48000,256,1},bank()); std::array<float,256> out{};
    check(bad_signal.process(std::array{on()},out).error==ProcessError::nonfinite_output,"non-finite sampler control must panic safely");
    for(float value:out) near(value,0,"bad sampler control leaked non-finite audio");
}

void invalid_resources() {
    auto p=instrument(); rejects([&]{ Engine e(p,{}); });
    rejects([&]{ Engine e(p,{},SampleBank{nullptr}); });
    auto bad=std::make_shared<SampleData>(); bad->left={1}; bad->right={1,2}; rejects([&]{ Engine e(p,{},SampleBank{bad}); });
    bad->right.clear(); bad->left[0]=std::numeric_limits<float>::infinity(); rejects([&]{ Engine e(p,{},SampleBank{bad}); });
    bad->left[0]=1; bad->sample_rate=0; rejects([&]{ Engine e(p,{},SampleBank{bad}); });
    p.nodes[8].resource_index=1; rejects([&]{ Engine e(p,{},bank()); });
    p=instrument(); p.nodes[8].flags=4; rejects([&]{ Engine e(p,{},bank()); });
    p=instrument(); p.nodes[8].channels=1; rejects([&]{ Engine e(p,{},bank()); });
}
}
int main() {
    try { basics(); rate_pitch_and_regions(); loop_and_trigger(); poly_and_reuse(); release_tail_and_heap(); resampling_quality_and_partition(); tiny_regions_and_extreme_controls(); invalid_resources();
        std::cout << "Sample engine tests passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
