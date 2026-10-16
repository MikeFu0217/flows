#include "engine/engine.hpp"
#include "compiler/program.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace flows;
void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
Program compileProgram(const std::string& body, bool mono = false) {
    auto source = "import standard as std\nfrom lib.osc import ClassicSine, ClassicSaw\nfrom lib.env import ClassicADSR\nfrom lib.sample import ClassicSampler\nInst.sigstream(std.event.keys." +
        std::string(mono ? "mono" : "poly") + "()) as key:\n" + body;
    auto result = compile(source, "boundary.flows");
    check(result.program.has_value(), "Boundary fixture must compile");
    return *result.program;
}
struct Audio { std::vector<float> left, right; };
Audio render(const Program& p, const std::vector<Event>& events, double rate, unsigned block, double correction, unsigned voices = 1,
             SampleBank bank = {}, CaptureSink* capture = nullptr) {
    Engine engine(p, {rate, block, voices, correction}, std::move(bank));
    Audio result{std::vector<float>(4096), std::vector<float>(4096)};
    std::size_t event = 0;
    for (std::size_t start = 0; start < result.left.size(); start += block) {
        const auto count = std::min<std::size_t>(block, result.left.size() - start);
        const auto begin = event;
        while (event < events.size() && events[event].sample < start + count) ++event;
        check(bool(engine.process_stereo(std::span(events).subspan(begin, event - begin),
            std::span(result.left).subspan(start, count), std::span(result.right).subspan(start, count), capture)), "Boundary render failed");
    }
    check(engine.stats().rejected_notes == 0, "Correction tail occupied a voice slot");
    return result;
}
double jump(const std::vector<float>& audio, std::size_t sample) { return std::abs(audio[sample] - (sample ? audio[sample - 1] : 0)); }
struct Tap final : CaptureSink {
    ValueId target = 0;
    std::array<float,4096> left{}, right{};
    void capture(ValueId, VoiceIdentity, std::uint64_t, std::span<const float>) noexcept override {}
    void capture_stereo(ValueId id, VoiceIdentity, std::uint64_t at, std::span<const float> l, std::span<const float> r) noexcept override {
        if (id != target) return;
        for (std::size_t i=0;i<l.size() && at+i<left.size();++i) { left[at+i]=l[i]; right[at+i]=r[i]; }
    }
    void event(const Event&, VoiceIdentity, std::string_view) noexcept override {}
};
int main() {
    try {
        const auto gate = compileProgram("    return key.gate * key.velocity\n");
        const std::vector<Event> rapid{{17,EventType::note_on,1,60,.8}, {703,EventType::note_off,1},
            {720,EventType::note_on,2,60,.4}, {731,EventType::note_off,2}, {733,EventType::note_on,3,60,1}, {1901,EventType::note_off,3},
            {2600,EventType::note_on,4,60,.6}, {3300,EventType::note_off,4}};
        for (double rate : {44100., 48000., 96000., 192000.}) {
            const auto raw = render(gate, rapid, rate, 256, 0);
            const auto smooth = render(gate, rapid, rate, 256, .003);
            const auto length = std::size_t(std::round(rate * .003));
            check(jump(raw.left,703) > .79, "Fixture no longer reproduces the original gate click");
            for (auto event : rapid) check(jump(smooth.left,event.sample) < 1e-6, "Event boundary is discontinuous");
            check(smooth.left[1901 + length] == 0, "Tail did not reach exact silence in 3 ms");
            check(smooth.left[17 + length] == raw.left[17 + length], "Correction altered the steady signal");
            for (auto block : {1u, 7u, 63u, 1024u}) {
                const auto partitioned = render(gate, rapid, rate, block, .003);
                check(partitioned.left == smooth.left && partitioned.right == smooth.right, "Correction depends on callback partition");
            }
        }
        const auto sine = compileProgram("    osc = ClassicSine.make()\n    raw = osc(pitch: key.pitch, level: -6db)\n    return raw * key.gate\n");
        const std::vector<Event> held{{0,EventType::note_on,1,48,1},{1501,EventType::note_off,1}};
        const auto raw = render(sine, held, 48000, 256, 0), smooth = render(sine, held, 48000, 256, .003);
        check(std::equal(raw.left.begin()+48,raw.left.begin()+1501,smooth.left.begin()+48), "Sine waveform changed after the 1 ms onset window");
        check(jump(raw.left,1501) > .2 && jump(smooth.left,1501) < 1e-6, "Sine release click was not repaired");
        double largest = 0; for (std::size_t i=1501;i<1646;++i) largest=std::max(largest,jump(smooth.left,i));
        check(largest < .006, "Release correction itself contains a sharp edge");

        const auto saw = compileProgram("    osc = ClassicSaw.make()\n    raw = osc(pitch: key.pitch, level: -6db)\n    return raw * key.gate\n");
        const auto sr = render(saw,held,48000,256,0), ss = render(saw,held,48000,256,.003);
        check(jump(sr.left,1)>.4 && jump(ss.left,1)<.001, "Zero-valued first sample hid a saw onset click");
        check(std::equal(sr.left.begin()+48,sr.left.begin()+1501,ss.left.begin()+48), "Onset window changed steady saw timbre");
        const auto mono = compileProgram("    env = ClassicADSR.make()\n    amp = env(gate: key.gate, trigger: key.trigger, A: 5ms, D: 0ms, S: 1, R: 20ms, retrigger_from: zero)\n    return amp * key.velocity\n",true);
        const std::vector<Event> overlap{{0,EventType::note_on,1,48,1},{701,EventType::note_on,2,60,.3},
            {809,EventType::note_off,2},{950,EventType::note_off,1}};
        const auto mr = render(mono,overlap,48000,256,0), ms = render(mono,overlap,48000,256,.003);
        check(jump(mr.left,701) > .9 && jump(ms.left,701) < 1e-6, "Mono retrigger was not repaired");
        check(jump(ms.left,809) < 1e-6, "Mono fallback velocity is discontinuous");
        check(ms.left[2054] == 0, "Authored release failed to finish");
        const auto mp = render(mono,overlap,48000,1,.003);
        check(mp.left == ms.left, "Mono envelope correction depends on callback partition");
        const auto continuous = render(compileProgram("    return 0.25\n",true),{{50,EventType::note_on,1,48,1},{1501,EventType::note_off,1}},48000,256,.003);
        check(std::all_of(continuous.left.begin(),continuous.left.end(),[](float x){return x==.25f;}),
              "A key added an onset window to an already audible ungated mono graph");
        for (bool monoSample : {false,true}) {
            const auto sampler = compileProgram("    player = ClassicSampler.make()\n    raw = player(file: \"Samples/test.wav\", trigger: key.trigger, pitch: key.pitch, root: C3, level: 1)\n    return raw\n",monoSample);
            auto pcm = std::make_shared<SampleData>(); pcm->left.assign(257,.5f); pcm->right.assign(257,-.25f);
            Tap before, after;
            for (const auto& name : sampler.named_values) if (name.name=="raw") before.target=after.target=name.value;
            const std::vector<Event> notes{{0,EventType::note_on,1,48,1},{901,EventType::note_off,1}};
            render(sampler,notes,48000,256,0,1,{pcm},&before);
            const auto s = render(sampler,notes,48000,256,.003,1,{pcm},&after);
            check(before.left==after.left && before.right==after.right && after.left[0]==.5f, "Playback correction modified a probe");
            check(s.left[0]==0 && s.left[48]==.5f && s.left[144]==.5f && s.left[257]==.5f && s.left[401]==0, "Sample onset/EOF correction timing is wrong");
            for (std::size_t i=0;i<s.left.size();++i) check(s.left[i]==-2*s.right[i], "Correction changed stereo balance");
            for (auto block : {1u,63u,1024u}) check(render(sampler,notes,48000,block,.003,1,{pcm}).left==s.left, "Sample EOF depends on callback partition");
        }
        const std::vector<Event> chord{{0,EventType::note_on,1,48,.8},{50,EventType::note_on,2,60,.3},
            {400,EventType::note_off,2},{703,EventType::note_off,1}};
        const auto combined=render(gate,chord,48000,256,.003,2);
        const auto first=render(gate,{chord[0],chord[3]},48000,256,.003);
        const auto second=render(gate,{chord[1],chord[2]},48000,256,.003);
        for (std::size_t i=0;i<combined.left.size();++i) check(combined.left[i]==first.left[i]+second.left[i], "One voice changed another voice's correction");
        std::cout << "Boundary audio: gate/reuse, sine, mono retrigger/fallback, stereo sample EOF, exact probes, independent polyphony, 4 sample rates and 5 block sizes passed.\n";
        std::cout << "Sine release jump: " << jump(raw.left,1501) << " -> " << jump(smooth.left,1501)
                  << "; largest step during tail: " << largest << '\n';
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
