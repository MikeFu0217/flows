#include "compiler/program.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace {
using namespace flows;
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
Program valid(const std::string& source) {
    auto result = compile(source, "Samples-test.flows");
    if (!result.program) throw std::runtime_error("Unexpected rejection: " + result.diagnostics.at(0).message);
    require(result.diagnostics.empty(), "Successful compile returned diagnostics.");
    return std::move(*result.program);
}
void invalid(const std::string& source, const std::string& expected) {
    auto result = compile(source, "Samples-test.flows");
    require(!result.program && !result.diagnostics.empty(), "Expected rejection: " + expected);
    const auto& error = result.diagnostics[0];
    require(error.message.find(expected) != std::string::npos, "Expected " + expected + ", got " + error.message);
    require(error.span.offset <= source.size() && error.span.length <= source.size() - error.span.offset, "Bad diagnostic span.");
}
std::string replace(std::string text, const std::string& from, const std::string& to) {
    auto offset = text.find(from); require(offset != std::string::npos, "Replacement missing."); text.replace(offset, from.size(), to); return text;
}
std::string instrument(const std::string& args = "", const std::string& result = "raw") {
    return "import standard as std\nfrom lib.sample import ClassicSampler\n"
        "Inst.sigstream(std.event.keys.poly()) as key:\n"
        "    sampler = ClassicSampler.make()\n"
        "    raw = sampler(file: \"Samples/kick.wav\", trigger: key.trigger" + args + ")\n"
        "    return " + result + "\n";
}
const Node& sampler(const Program& program) {
    const auto found = std::find_if(program.nodes.begin(), program.nodes.end(), [](const auto& node) { return node.op == Op::sampler; });
    require(found != program.nodes.end(), "Missing sampler node."); return *found;
}
const ChoiceDescriptor& choice(const Program& program, const std::string& name) {
    const auto found = std::find_if(program.choices.begin(), program.choices.end(), [&](const auto& item) { return item.owner == "raw" && item.name == name; });
    require(found != program.choices.end(), "Missing sampler choice " + name); return *found;
}
const ChoiceOption& option(const ChoiceDescriptor& choice, const std::string& value) {
    const auto found = std::find_if(choice.options.begin(), choice.options.end(), [&](const auto& item) { return item.value == value; });
    require(found != choice.options.end(), "Missing option."); return *found;
}
void enabled_choices_compile(const std::string& source) {
    auto program = valid(source);
    for (const auto& choice : program.choices) for (const auto& option : choice.options) if (option.enabled) {
        auto edited = source;
        edited.replace(choice.span.offset, choice.span.length, choice.prefix + option.value + choice.suffix);
        valid(edited);
    }
}
void basics_and_lifetime() {
    auto source = instrument(); auto program = valid(source); const auto& node = sampler(program);
    require(program.states.size() == 1 && program.states[0].kind == StateKind::sampler, "Sampler requires independent voice state.");
    require(node.channels == 2 && node.inputs.size() == 8 && node.resource_index == 0 && node.flags == 0, "Invalid sampler graph contract.");
    require(program.samples.size() == 1 && program.samples[0].path == "Samples/kick.wav", "Resource path missing.");
    const auto span = program.samples[0].span;
    require(source.substr(span.offset, span.length) == "\"Samples/kick.wav\"", "Resource origin span must include the quoted path.");
    require(program.completion_root && program.completion[*program.completion_root].op == CompletionOp::sample_done, "One-shot must end at EOF independently of key gate.");
    require(program.nodes[node.inputs[1]].initial_value == 48 && program.nodes[node.inputs[2]].initial_value == 48, "Default pitch and root must be C3.");
    require(!option(choice(program,"mode"),"loop").enabled, "Ungated loop cannot replace the only poly completion witness.");
    enabled_choices_compile(source);
    auto loop = valid(instrument(", mode: loop, gate: key.gate, reverse: true"));
    require(sampler(loop).flags == 3 && loop.completion_root && loop.completion[*loop.completion_root].op == CompletionOp::gate_closed, "Explicit loop gate must own completion.");
    require(!option(choice(loop,"mode"),"one_shot").enabled, "one_shot must not ignore an explicit gate.");
    enabled_choices_compile(instrument(", mode: loop, gate: key.gate, reverse: true"));
    auto gated = instrument("", "raw * key.gate");
    require(option(choice(valid(gated),"mode"),"loop").enabled, "Outer gate makes looping safe."); enabled_choices_compile(gated);
    auto envelope = replace(instrument(", mode: loop", "raw * amp"), "from lib.sample", "from lib.env import ClassicADSR\nfrom lib.sample");
    envelope = replace(envelope, "    return", "    env = ClassicADSR.make()\n    amp = env(gate: key.gate, trigger: key.trigger, R: 250ms)\n    return");
    const auto released = valid(envelope);
    require(released.completion_root && released.completion[*released.completion_root].op == CompletionOp::envelope_idle, "An ungated loop must preserve the outer ADSR release.");
    enabled_choices_compile(envelope);
    invalid(instrument(", mode: loop"), "lifetime is unproven");
    invalid(instrument(", gate: key.gate"), "gate is only used in loop");
    invalid(replace(source, ", trigger: key.trigger", ""), "requires 'trigger'");
    auto with_probes = valid(replace(source, "    return", "    p = probe(raw)\n    energy = power(p)\n    return"));
    require(with_probes.named_analysis.size() == 2 && dsp_structure_key(with_probes) == dsp_structure_key(program), "Sampler probes must not replace the audio graph.");
}
void composition_and_resources() {
    const std::string source =
        "import standard as std\nfrom lib.sample import ClassicSampler\n"
        "kick = \"Samples/kick.wav\"\n"
        "class Layer:\n"
        "    def signal(file: sample, trigger: control, pitch: pitch):\n"
        "        sampler = ClassicSampler.make()\n"
        "        return sampler(file: file, trigger: trigger, pitch: pitch)\n"
        "Inst.sigstream(std.event.keys.poly()) as key:\n"
        "    one = Layer.make()\n    two = Layer.make()\n"
        "    local = kick\n"
        "    a = one(file: local, trigger: key.trigger, pitch: key.pitch)\n"
        "    b = two(file: \"Samples/kick.wav\", trigger: key.trigger, pitch: key.pitch + 12)\n"
        "    return (a + b) * key.velocity\n";
    const auto program = valid(source);
    require(program.samples.size() == 1 && program.states.size() == 2, "Shared file data must retain independent playback state.");
    require(program.states[0].name == "one.sampler" && program.states[1].name == "two.sampler", "Composition must qualify sampler state.");
    require(std::count_if(program.nodes.begin(),program.nodes.end(),[](const auto& n){ return n.op == Op::sampler && n.resource_index == 0; }) == 2, "Both sample readers must reuse their resource descriptor.");
    auto default_source = replace(source,"file: sample,", "file: sample = kick,");
    default_source = replace(default_source,"file: local, ",""); valid(default_source);
    auto pitch_default = replace(default_source,"pitch: pitch)","pitch: pitch = C3)");
    pitch_default = replace(pitch_default, "trigger: key.trigger, pitch: key.pitch)", "trigger: key.trigger)");
    valid(pitch_default);
    invalid(replace(source,"file: local,", "file: 1,"), "Sample ports require");
    invalid(replace(source,"pitch: key.pitch)", "pitch: kick)"), "Class ports require signal values");
    auto other = valid(replace(source,"b = two(file: \"Samples/kick.wav\"", "b = two(file: \"Samples/snare.wav\""));
    require(other.samples.size() == 2, "Different paths require separate resources.");
    auto unused = valid(replace(instrument(),"Inst.sigstream", "unused = \"Samples/missing.wav\"\nInst.sigstream"));
    require(unused.samples.size() == 1, "Unused aliases must not load resources.");
    auto subclass = replace(instrument(", level: -6db"), "Inst.sigstream", "class Quiet(ClassicSampler):\n    def init:\n        self.register_knob(level, default=-12db, range=(-48db, 0db), step=0.1db)\nInst.sigstream");
    subclass = replace(subclass,"sampler = ClassicSampler", "sampler = Quiet");
    auto registered = valid(subclass);
    require(std::any_of(registered.parameters.begin(),registered.parameters.end(),[](const auto& p){ return p.name=="level" && p.has_default && p.default_value==-12 && p.min==-48 && p.max==0; }), "Sampler subclasses must register knob metadata.");
    auto tuned = replace(subclass,"    def init:\n", "    def init:\n        self.register_knob(root, default=C3, range=(C1, C6))\n");
    valid(tuned);
}
void paths_and_diagnostics() {
    const auto base = instrument();
    for (const auto& path : {"", "/tmp/kick.wav", "C:/kick.wav", "https://example.com/kick.wav", "../kick.wav", "Samples/../kick.wav", "./kick.wav", "Samples//kick.wav", "Samples/kick.wav/"})
        invalid(replace(base,"Samples/kick.wav",path),"Sample paths");
    invalid(replace(base,"Samples/kick.wav","Samples\\\\kick.wav"),"Sample paths");
    invalid(replace(base,"Samples/kick.wav","Samples\\nkick.wav"),"String escapes");
    invalid(replace(base,"Samples/kick.wav","Samples/\nkick.wav"),"control characters");
    invalid(replace(base,"Samples/kick.wav",std::string(4097,'a')),"exceeds 4096");
    invalid(replace(base,"\"Samples/kick.wav\"","\"Samples/kick.wav"),"control characters");
    auto escaped = valid(replace(base,"Samples/kick.wav","Samples/the \\\"kick\\\".wav"));
    require(escaped.samples[0].path == "Samples/the \"kick\".wav", "Quoted filename escapes must decode exactly.");
    require(program_json(escaped).find("the \\\"kick\\\".wav") != std::string::npos, "Resource JSON must escape paths.");
    valid(replace(base,"\"Samples/kick.wav\"","'Samples/kick.wav'"));
    invalid(instrument(", mode: pingpong"), "one_shot or loop");
    invalid(instrument(", reverse: key.gate"), "constant true or false");
    invalid(instrument(", rate: 0"), "outside its supported range");
    invalid(instrument(", rate: 2hz"), "requires scalar");
    invalid(instrument(", pitch: 440hz"), "requires pitch");
    invalid(instrument(", root: 48"), "requires pitch");
    invalid(instrument(", start: .8, end: .2"), "start must be less than end");
    invalid(instrument(", start: .5, end: .5"), "start must be less than end");
    invalid(instrument(", start: -0.1"), "outside its supported range");
    invalid(instrument(", end: 1.1"), "outside its supported range");
    invalid(replace(base,"file: \"Samples/kick.wav\"", "file: key.pitch"), "constant sample path");
    invalid(replace(base,"    return raw", "    again = sampler(file: \"Samples/kick.wav\", trigger: key.trigger)\n    return raw"), "only once");
    invalid(replace(base,"    return raw", "    if key.gate:\n        file = \"Samples/kick.wav\"\n    else:\n        file = \"Samples/snare.wav\"\n    return raw"), "outside conditional");
}
void identities_and_metadata() {
    auto source = instrument(", pitch: key.pitch, root: C3, rate: 1, start: 0, end: 1, level: -6db, reverse: false");
    const auto base = valid(source); const auto key = dsp_structure_key(base);
    for (const auto& [from,to] : {std::pair{"level: -6db","level: -12db"}, {"rate: 1","rate: 2"}, {"start: 0","start: .1"}, {"end: 1","end: .8"}, {"root: C3","root: C4"}})
        require(dsp_structure_key(valid(replace(source,from,to))) == key, "Live sampler edits must preserve voice state.");
    require(dsp_structure_key(valid("# moved\n" + source)) == key, "Resource source spans must not change DSP identity.");
    require(dsp_structure_key(valid(replace(source,"kick.wav","snare.wav"))) != key, "Replacing a sample is structural.");
    require(dsp_structure_key(valid(replace(source,"reverse: false","reverse: true"))) != key, "Changing direction is structural.");
    std::set<std::string> names; for (const auto& p : base.parameters) { names.insert(p.name); require(p.span.offset+p.span.length<=source.size(),"Bad parameter span."); }
    for (const auto& name : {"root","rate","start","end","level"}) require(names.contains(name),"Sampler numeric metadata missing.");
    auto json = program_json(base);
    for (const auto& field : {"\"sampler\"", "\"samples\"", "\"resource_index\"", "\"sample_done\""}) require(json.find(field)!=std::string::npos,"Sampler JSON metadata missing.");
    enabled_choices_compile(source);
}
void resource_budget() {
    std::string source = "import standard as std\nfrom lib.sample import ClassicSampler\nInst.sigstream(std.event.keys.poly()) as key:\n";
    for (int i = 0; i < 64; ++i) {
        const auto id = std::to_string(i);
        source += "    s" + id + " = ClassicSampler.make()\n    v" + id + " = s" + id + "(file: \"Samples/" + id + ".wav\", trigger: key.trigger)\n";
    }
    require(valid(source + "    return v0\n").samples.size() == 64, "64 unique resources must fit the engine budget.");
    invalid(source + "    overflow = ClassicSampler.make()\n    more = overflow(file: \"Samples/64.wav\", trigger: key.trigger)\n    return more\n", "exceeds 64 sample resources");
}
}
int main() {
    try {
        basics_and_lifetime(); composition_and_resources(); paths_and_diagnostics(); identities_and_metadata(); resource_budget();
        std::cout << "Sample compiler tests passed: resources, composition, lifetime, metadata, choices and path diagnostics.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "Sample compiler test failure: " << error.what() << '\n'; return 1; }
}
