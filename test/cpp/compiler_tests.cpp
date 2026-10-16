#include "compiler/program.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>

namespace {
using namespace flows;
void require(bool condition,const std::string& message){if(!condition)throw std::runtime_error(message);}
std::string instrument(const std::string& body){return "import standard as std\nfrom lib.osc import ClassicSine\n\nInst.sigstream(std.event.keys.poly()) as key:\n"+body;}
Program valid(const std::string& source){auto result=compile(source,"compiler-test.flows");if(!result.program)throw std::runtime_error("Unexpected rejection at "+std::to_string(result.diagnostics[0].span.line)+": "+result.diagnostics[0].message);require(result.diagnostics.empty(),"Success returned diagnostics.");return std::move(*result.program);}
void invalid(const std::string& source,const std::string& expected){auto result=compile(source,"compiler-test.flows");require(!result.program,"Expected rejection: "+expected);require(!result.diagnostics.empty(),"Missing diagnostic.");require(result.diagnostics[0].message.find(expected)!=std::string::npos,"Expected '"+expected+"', got: "+result.diagnostics[0].message);const auto span=result.diagnostics[0].span;require(span.offset<=source.size()&&span.length<=source.size()-span.offset,"Diagnostic span exceeds source.");require(span.line>=1&&span.column>=1,"Positions must be one-based.");}
ValueId named(const Program& program,const std::string& name){for(const auto& value:program.named_values)if(value.name==name)return value.value;throw std::runtime_error("Missing signal "+name);}
std::string read(const std::filesystem::path& path){std::ifstream input(path);require(input.good(),"Cannot read "+path.string());return {(std::istreambuf_iterator<char>(input)),{}};}
std::string replace(std::string source,const std::string& from,const std::string& to){auto pos=source.find(from);require(pos!=std::string::npos,"Test replacement not found.");source.replace(pos,from.size(),to);return source;}
void structural_invariants(const Program& program,const std::string& source){
    for(std::size_t i=0;i<program.nodes.size();++i){for(auto input:program.nodes[i].inputs)require(input<i,"DSP graph must be topological.");auto span=program.nodes[i].span;require(span.offset<=source.size()&&span.length<=source.size()-span.offset,"DSP span exceeds source.");}
    std::set<std::string> bindings;for(const auto& binding:program.parameters){require(bindings.insert(binding.binding_id).second,"Binding identities must be unique.");require(binding.value<program.nodes.size()&&program.nodes[binding.value].op==Op::parameter,"Binding must reference a parameter slot.");}
    for(std::size_t i=0;i<program.completion.size();++i)for(auto input:program.completion[i].inputs)require(input<i,"Completion graph must be topological.");
}
const std::string sine_body="    osc = ClassicSine.make()\n    raw = osc(pitch: key.pitch, unison: 1, level: -6db)\n    return raw * key.gate * key.velocity\n";
void sine_regression(){
    const auto source=instrument(sine_body);const auto p=valid(source);structural_invariants(p,source);require(p.states.size()==1,"One oscillator must have one state.");const auto& oscillator=p.nodes[named(p,"raw")];require(oscillator.op==Op::sine&&oscillator.state_index==0,"Oscillator state missing.");require(p.nodes[oscillator.inputs[0]].op==Op::pitch_to_hz,"Explicit pitch conversion missing.");const auto& gain=p.nodes[oscillator.inputs[1]];require(gain.op==Op::db_to_linear,"Explicit dB conversion missing.");const auto& literal=p.nodes[gain.inputs[0]];require(literal.initial_value==-6,"Literal must remain a parameter.");require(source.substr(literal.span.offset,literal.span.length)=="-6db","Negative literal span incorrect.");require(p.completion_root.has_value(),"Gated output must have lifetime proof.");require(p.voice_policy.mode==VoiceMode::poly,"Default policy is poly.");
    auto unison=std::find_if(p.parameters.begin(),p.parameters.end(),[](const auto& parameter){return parameter.name=="unison";});
    require(unison!=p.parameters.end()&&unison->has_range&&unison->min==1&&unison->max==8&&unison->step==1&&unison->discrete&&!unison->live,"Unison must expose integer counts 1..8 and require a rebuild.");
    require(source.substr(unison->span.offset,unison->span.length)=="1","Fixed unison metadata lost the authored source span.");
    auto aliases=valid("# 中文 comment\r\nimport standard as events\r\nfrom lib.osc import ClassicSine as Sine\r\nvoices = events.event.keys.poly()\r\nInst.sigstream(voices) as note:\r\n  osc = Sine.make()\r\n  frequency = note.pitch + 12\r\n  raw = osc(\r\n    pitch: frequency, # octave\r\n    level: -(3db + 3db),\r\n  )\r\n  copy = raw\r\n  gated = copy * note.gate\r\n  return (gated + gated) * note.velocity / (2 * 2 - 2)");
    require(named(aliases,"raw")==named(aliases,"copy"),"Aliases must reuse the same computed value.");require(aliases.nodes[named(aliases,"frequency")].unit==Unit::pitch,"Pitch offset must preserve pitch unit.");
    auto separate=valid(instrument("    one = ClassicSine.make()\n    two = ClassicSine.make()\n    a = one(pitch: 0.44khz, level: 5e-1)\n    b = two(pitch: 440hz)\n    return (a - b) * key.gate\n"));
    require(separate.states.size()==2&&separate.nodes[named(separate,"a")].state_index!=separate.nodes[named(separate,"b")].state_index,"Independent .make() instances must not share state.");
    auto notes=valid(instrument("    osc = ClassicSine.make()\n    raw = osc(pitch: F#1) # a sharp is part of the note\n    return raw * key.gate\n"));
    require(std::any_of(notes.nodes.begin(),notes.nodes.end(),[](const auto& node){return node.op==Op::parameter&&node.unit==Unit::pitch&&node.initial_value==30;}),"Sharp note literal must follow MIDI octave numbering.");
}
void repository_examples(){
    const auto root=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const std::set<std::string> expected{"sine-reference.flows","probe-lab.flows","mono-legato-glide.flows","trance-rolling-bass.flows","stereo-motion.flows","sample-one-shot.flows","sample-loop.flows"};
    std::set<std::string> presets,sources;std::set<Op> operations;std::set<Unit> units;
    for(const auto& file:std::filesystem::directory_iterator(root/"test/presets"))if(file.path().extension()==".flows"){
        auto source=read(file.path());auto p=valid(source);structural_invariants(p,source);
        presets.insert(file.path().filename().string());sources.insert(source);
        for(const auto& node:p.nodes){operations.insert(node.op);units.insert(node.unit);}
    }
    require(presets==expected,"Keep exactly the seven complementary public instrument examples.");
    for(auto op:{Op::sampler,Op::unison_saw,Op::lfo,Op::distortion,Op::sine,Op::saw,Op::adsr,Op::filter_lowpass,Op::filter_highpass,Op::add,Op::subtract,Op::multiply,Op::divide,Op::select,Op::less_equal,Op::key_pitch,Op::key_gate,Op::key_velocity,Op::key_trigger})
        require(operations.contains(op),"Public examples lost DSP coverage for "+std::string(op_name(op)));
    for(auto unit:{Unit::scalar,Unit::pitch,Unit::hertz,Unit::decibels,Unit::seconds,Unit::boolean})
        require(units.contains(unit),"Public examples lost typed expression coverage.");
    std::size_t fixtures=0;
    for(const auto& file:std::filesystem::directory_iterator(root/"test/fixtures/instruments"))if(file.path().extension()==".flows"){
        auto source=read(file.path());structural_invariants(valid(source),source);++fixtures;
    }
    require(fixtures==3,"Retain the three focused internal regression instruments.");
    auto doc=read(root/"docs/LANGUAGE.md");std::size_t cursor=0;std::set<std::string> documented;
    while((cursor=doc.find("```flows\n",cursor))!=std::string::npos){cursor+=9;auto end=doc.find("```",cursor);require(end!=std::string::npos,"Unclosed documented example.");auto source=doc.substr(cursor,end-cursor);valid(source);require(documented.insert(source).second,"Duplicate documented instrument.");cursor=end+3;}
    require(documented==sources,"The seven complete language examples must match their playable presets exactly.");
    auto bass=valid(read(root/"test/presets/trance-rolling-bass.flows"));require(bass.voice_policy.mode==VoiceMode::mono&&bass.voice_policy.retrigger==Retrigger::each_press,"Rolling policy incorrect.");require(bass.states.size()==7,"Rolling bass needs two oscillators, two filters and three envelopes.");require(bass.completion_root&&bass.completion[*bass.completion_root].op==CompletionOp::all,"Both layer envelopes must complete.");
    require(std::count_if(bass.nodes.begin(),bass.nodes.end(),[](const auto& node){return node.op==Op::adsr&&(node.flags&retrigger_from_zero);})==3,"Rolling bass must demonstrate all three envelopes restarting from zero.");
    auto glide=valid(read(root/"test/presets/mono-legato-glide.flows"));require(glide.voice_policy.glide_seconds==.1&&glide.voice_policy.retrigger==Retrigger::gate_rise,"Glide/retrigger policy lost.");require(glide.nodes[named(glide,"key.pitch")].rate==Rate::sample_varying,"Glide pitch is sample-varying.");
    auto poly=valid(read(root/"test/fixtures/instruments/poly-adsr.flows"));require(poly.completion_root&&poly.completion[*poly.completion_root].op==CompletionOp::envelope_idle,"Envelope must preserve release tail.");

    auto labSource=read(root/"test/presets/probe-lab.flows");auto lab=valid(labSource);
    require(lab.voice_policy.mode==VoiceMode::poly&&lab.states.size()==3,"Probe lab must keep independent oscillator/filter/envelope state in each poly voice.");
    require(std::any_of(lab.states.begin(),lab.states.end(),[](const auto& state){return state.name=="filt.stage"&&state.kind==StateKind::filter;}),"Typed composition must expand its inner filter state.");
    require(lab.completion_root&&lab.completion[*lab.completion_root].op==CompletionOp::envelope_idle,"Probe lab must retain its amplitude Release.");
    require(lab.nodes[named(lab,"amp")].op==Op::adsr&&!(lab.nodes[named(lab,"amp")].flags&retrigger_from_zero),"Probe lab must demonstrate retrigger_from: current.");
    auto cutoff=std::find_if(lab.parameters.begin(),lab.parameters.end(),[](const auto& parameter){return parameter.name=="cutoff";});
    require(cutoff!=lab.parameters.end()&&cutoff->has_default&&cutoff->default_value==900&&cutoff->min==80&&cutoff->max==12000&&cutoff->step==25&&cutoff->logarithmic&&cutoff->live,"Inherited knob range/step/scale must reach the composed filter port.");
    auto resonance=std::find_if(lab.parameters.begin(),lab.parameters.end(),[](const auto& parameter){return parameter.name=="resonance";});
    require(resonance!=lab.parameters.end()&&resonance->has_default&&resonance->default_value==.2&&lab.nodes[resonance->value].initial_value==.2,"Registered composition default must override its port fallback.");
    std::set<AnalysisOp> analysisOps;std::set<AnalysisShape> shapes;std::set<ValueId> captureRoots;std::set<std::string> analysisNames;
    for(const auto& value:lab.named_analysis)analysisNames.insert(value.name);
    require(analysisNames==std::set<std::string>{"p_before","p_after","p_amp","amp_power","diff","spectrum_after","mel_diff","power_diff","power_percent"},"Probe lab must expose its audio/control probes and derived results.");
    for(const auto& node:lab.analysis){analysisOps.insert(node.op);shapes.insert(node.shape);if(node.op==AnalysisOp::probe)captureRoots.insert(node.signal);}
    for(auto op:{AnalysisOp::probe,AnalysisOp::add,AnalysisOp::subtract,AnalysisOp::multiply,AnalysisOp::divide,AnalysisOp::power,AnalysisOp::spectrum,AnalysisOp::mel_spectrum})
        require(analysisOps.contains(op),"Probe lab lost an analysis operation.");
    require(shapes==std::set<AnalysisShape>{AnalysisShape::waveform,AnalysisShape::scalar,AnalysisShape::spectrum,AnalysisShape::mel_spectrum},"Probe lab must demonstrate each analysis shape.");
    require(captureRoots==std::set<ValueId>{named(lab,"raw"),named(lab,"filtered"),named(lab,"amp")},"Probe lab must share exactly three capture roots, including a control signal.");
    auto observation=labSource.find("    # Observation:");auto output=labSource.find("    return signal",observation);
    require(observation!=std::string::npos&&output!=std::string::npos,"Probe lab observation block missing.");
    labSource.erase(observation,output-observation);
    require(dsp_structure_key(lab)==dsp_structure_key(valid(labSource)),"Adding probe lab observations must leave its playable DSP unchanged.");
}
void conditional_and_completion(){
    auto source=instrument("    osc = ClassicSine.make()\n    raw = osc(pitch: key.pitch)\n    if key.pitch <= C1:\n        signal = raw * 0.5\n    else:\n        signal = raw * 0.25\n    return signal * key.gate\n");auto p=valid(source);structural_invariants(p,source);require(p.nodes[named(p,"signal")].op==Op::select,"Pure conditional must lower to select.");
    invalid(instrument("    osc = ClassicSine.make()\n    if key.gate:\n        signal = osc(pitch: key.pitch)\n    else:\n        signal = 0\n    return signal * key.gate\n"),"Stateful calls");
    invalid(instrument("    if key.gate:\n        a = 1\n    else:\n        b = 2\n    return a * key.gate\n"),"Both if/else branches");
    const std::string imports="import standard as std\nfrom lib.osc import ClassicSine\nfrom lib.env import ClassicADSR\nfrom lib.filt import ClassicFilt\nInst.sigstream(std.event.keys.poly()) as key:\n    osc = ClassicSine.make()\n    env = ClassicADSR.make()\n    filt = ClassicFilt.make()\n    raw = osc(pitch: key.pitch)\n    amp = env(gate: key.gate, trigger: key.trigger)\n";
    auto before=valid(imports+"    filtered = filt(raw, cutoff: 800hz)\n    return filtered * amp\n");require(before.completion[before.completion_root.value()].op==CompletionOp::envelope_idle,"Filter before final envelope must not extend completion.");
    auto after=valid(imports+"    filtered = filt(raw * amp, cutoff: 800hz)\n    return filtered\n");auto root=after.completion[after.completion_root.value()];require(root.op==CompletionOp::all,"Filter after envelope needs input completion and settled state.");require(std::any_of(root.inputs.begin(),root.inputs.end(),[&](auto id){return after.completion[id].op==CompletionOp::filter_settled;}),"Filter tail witness missing.");
}
void observations_and_identity(){
    const auto source=instrument(sine_body);const auto baseline=valid(source);auto with_analysis=replace(source,"    return", "    p = probe(raw)\n    q = probe(key.gate)\n    difference = p - q\n    spectral = mel_spec(p) - mel_spec(q)\n    energy = power(p) - power(q)\n    scaled = 2 * p\n    return");auto observed=valid(with_analysis);
    require(observed.named_analysis.size()==6,"Analysis expressions missing.");require(dsp_structure_key(baseline)==dsp_structure_key(observed),"Opening probe/analysis must not replace the DSP program.");
    auto edited=valid(replace(source,"-6db","-12db"));require(dsp_structure_key(baseline)==dsp_structure_key(edited),"Live level change must preserve graph/state identity.");
    auto moved=valid("# shifted source location\n"+source);require(dsp_structure_key(baseline)==dsp_structure_key(moved),"Comments and positions must not alter DSP identity.");
    auto renamed=valid(replace(replace(source,"osc =","newosc ="),"raw = osc(","raw = newosc("));require(dsp_structure_key(baseline)!=dsp_structure_key(renamed),"State identity change must not reuse old state blindly.");
    auto mono=valid(replace(source,"keys.poly()","keys.mono(priority: last, retrigger: gate_rise, glide: 100ms)"));require(dsp_structure_key(baseline)!=dsp_structure_key(mono),"Voice policy must be structural.");
    invalid(replace(source,"    return raw * key.gate * key.velocity", "    p = probe(raw)\n    return p"),"cannot feed");
    invalid(replace(source,"    return", "    p = probe(raw * 2)\n    return"),"existing signal");
    invalid(replace(source,"    raw = osc(pitch: key.pitch, unison: 1, level: -6db)", "    p = probe(osc(pitch: key.pitch))\n    raw = 0"),"existing DSP signal");
    invalid(replace(source,"    return", "    p = probe((raw * 2))\n    return"),"existing signal name");
    invalid(replace(source,"    return", "    p = probe(raw)\n    bad = mel_spec(p) - p\n    return"),"compatible shapes");
    auto json=program_json(observed);require(json.find("\"parameters\"")!=std::string::npos&&json.find("\"completion_root\"")!=std::string::npos&&json.find("\"mel_spectrum\"")!=std::string::npos,"JSON metadata missing.");
}
void classes_and_metadata(){
    const std::string source=
        "import standard as std\nfrom lib.osc import ClassicSine\n"
        "class Layer:\n"
        "    def init:\n"
        "        self.register_knob(level, default=-12db, range=(-48db, 0db), step=0.1db, scale=linear, update=live)\n"
        "    def signal(pitch: pitch, gate: scalar, level: db = -6db):\n"
        "        osc = ClassicSine.make()\n"
        "        raw = osc(pitch: pitch, level: level)\n"
        "        return raw * gate\n"
        "Inst.sigstream(std.event.keys.poly()) as key:\n"
        "    one = Layer.make()\n"
        "    two = Layer.make()\n"
        "    a = one(pitch: key.pitch, gate: key.gate)\n"
        "    b = two(pitch: key.pitch + 12, gate: key.gate, level: -18db)\n"
        "    return (a + b) * key.velocity\n";
    auto p=valid(source);structural_invariants(p,source);require(p.states.size()==2&&p.states[0].name=="one.osc"&&p.states[1].name=="two.osc","Static class expansion must qualify independent state identities.");
    auto parameter=std::find_if(p.parameters.begin(),p.parameters.end(),[](const auto& item){return item.binding_id=="one.level.default.literal0";});require(parameter!=p.parameters.end(),"Default class port binding missing.");require(parameter->min==-48&&parameter->max==0&&parameter->unit==Unit::decibels&&parameter->default_value==-12,"Class metadata must control range and default.");require(p.nodes[parameter->value].initial_value==-12,"Registered default must initialize the omitted port.");
    require(parameter->smoothing_seconds==.005,"Direct gain binding needs smoothing metadata.");
    invalid(replace(source,"level: -18db","level: 800hz"),"requires decibels");
    invalid(replace(source,"level: -18db","unknown: -18db"),"Unknown class port");
    auto inherited=valid("import standard as std\nfrom lib.osc import ClassicSine\nclass Quiet(ClassicSine):\n    def init:\n        self.register_knob(level, default=-18db, range=(-60db, 0db), step=1db)\nInst.sigstream(std.event.keys.poly()) as key:\n    osc = Quiet.make()\n    raw = osc(pitch: key.pitch)\n    return raw * key.gate\n");require(std::any_of(inherited.parameters.begin(),inherited.parameters.end(),[](const auto& item){return item.default_value==-18&&item.has_range;}),"Builtin subclass metadata missing.");
    invalid("import standard as std\nclass Loop:\n    def signal(gate: scalar):\n        x = Loop.make()\n        return x(gate: gate)\nInst.sigstream(std.event.keys.poly()) as key:\n    x = Loop.make()\n    return x(gate: key.gate)\n","Recursive class");
}
void resource_budgets(){
    invalid(instrument("    "+std::string(257,'x')+" = key.gate\n    return key.gate\n"),"Identifier exceeds 256");
    std::string tokens;for(int i=0;i<131073;++i)tokens+="x ";invalid(tokens,"128 Ki tokens");
    std::string classes="import standard as std\nclass Layer0:\n    def signal(gate: scalar):\n        return gate\n";
    for(int i=1;i<16;++i){auto n=std::to_string(i),previous=std::to_string(i-1);classes+="class Layer"+n+":\n    def signal(gate: scalar):\n        a = Layer"+previous+".make()\n        b = Layer"+previous+".make()\n        x = a(gate: gate)\n        y = b(gate: gate)\n        return x\n";}
    classes+="Inst.sigstream(std.event.keys.poly()) as key:\n    top = Layer15.make()\n    return top(gate: key.gate)\n";
    invalid(classes,"2048 class expansions");
    std::string aliases;for(int i=0;i<8193;++i)aliases+="    a"+std::to_string(i)+" = key.gate\n";
    invalid(instrument(aliases+"    return key.gate\n"),"8192 named signals");
    std::string makes="import standard as std\nclass Pass:\n    def signal(gate: scalar):\n        return gate\nInst.sigstream(std.event.keys.poly()) as key:\n";
    for(int i=0;i<8193;++i)makes+="    a"+std::to_string(i)+" = Pass.make()\n";
    invalid(makes+"    return key.gate\n","8192 statements");
    std::string analysis="    p = probe(key.gate)";for(int i=0;i<8193;++i)analysis+=" + 0";
    invalid(instrument(analysis+"\n    return key.gate\n"),"16384 analysis nodes");
}
void diagnostics(){
    const std::string make="    osc = ClassicSine.make()\n";const std::string raw="    raw = osc(pitch: key.pitch, level: -6db)\n";
    invalid(instrument(make+"    return raw * key.gate\n"),"use before definition");
    invalid(instrument(make+raw+"    again = osc(pitch: key.pitch)\n    return again * key.gate\n"),"only once");
    invalid(instrument(make+"    raw = osc(pitch: key.pitch, level: osc(pitch: key.pitch))\n    return raw * key.gate\n"),"only once");
    invalid(instrument(make+"    raw = osc(pitch: key.pitch, level: -6hz)\n    return raw * key.gate\n"),"level: requires");
    invalid(instrument(make+"    raw = osc(pitch: 440)\n    return raw * key.gate\n"),"pitch: requires");
    invalid(instrument(make+"    raw = osc(pitch: key.pitch, unison: 9)\n    return raw * key.gate\n"),"constant integer");
    invalid(instrument(make+"    raw = osc(pitch: key.pitch, warp: 1)\n    return raw * key.gate\n"),"Unknown instance parameter");
    invalid(instrument(make+"    raw = osc(pitch: key.pitch, pitch: key.pitch)\n    return raw * key.gate\n"),"Duplicate instance parameter");
    invalid(instrument(make+"    raw = osc(level: 1)\n    return raw * key.gate\n"),"requires 'pitch'");
    invalid(instrument(make+"    raw = osc(pitch: 20kg)\n    return raw * key.gate\n"),"Unsupported unit");
    invalid(instrument(make+"    raw = osc(pitch: key.pitch + 440hz)\n    return raw * key.gate\n"),"Cannot add/subtract");
    invalid(instrument(make+"    raw = osc(pitch: -1hz)\n    return raw * key.gate\n"),"non-negative");
    invalid(instrument(make+raw+"    return raw\n"),"lifetime is unproven");
    invalid(instrument(make+raw+"    return raw * key.gate + 0.1\n"),"lifetime is unproven");
    invalid(instrument(make+raw+"    return raw / key.gate * key.gate\n"),"constant-rate divisor");
    invalid(instrument(make+raw+"    return raw * key.gate / (1 - 1)\n"),"constant-rate divisor");
    invalid(instrument(make+raw+"    raw = 1\n    return raw * key.gate\n"),"already defined");
    invalid(instrument(make),"requires a return");
    invalid(instrument(make+raw+"    return raw * key.gate\n    late = 1\n"),"after return");
    invalid(instrument("\treturn 1\n"),"spaces instead of tabs");
    invalid(instrument(make+"   return key.gate\n"),"Indentation");
    invalid(instrument(make+"    return (key.gate\n"),"Unclosed");
    invalid(instrument(make+"    return key.gate)\n"),"Unexpected ')'");
    invalid(instrument(make+"    x = 1e999\n    return x * key.gate\n"),"finite");
    invalid(instrument(make+"    x = 1e\n    return x * key.gate\n"),"numeric exponent");
    invalid(instrument(make+"    x = 1e308khz\n    return x * key.gate\n"),"finite");
    invalid(instrument(make+"    raw = osc(pitch: key.pitch, level: 1e308db)\n    return raw * key.gate\n"),"non-finite");
    invalid("", "Expected one Inst.sigstream");
    invalid(instrument("    return key.gate\n")+"x = 1\n","Only one Inst.sigstream");
    invalid(instrument("    return "+std::string(300,'(')+"key.gate"+std::string(300,')')+"\n"),"nesting exceeds");
}
double pitch_literal(const std::string& literal){
    const auto program=valid(instrument("    osc = ClassicSine.make()\n    raw = osc(pitch: "+literal+", level: -6db)\n    return raw * key.gate\n"));
    const auto& oscillator=program.nodes[named(program,"raw")];
    // A hertz literal feeds the oscillator directly; a pitch-unit literal is
    // wrapped in an explicit pitch_to_hz node first.
    const auto& input=program.nodes[oscillator.inputs[0]];
    return input.inputs.empty()?input.initial_value:program.nodes[input.inputs[0]].initial_value;
}
// The compiler parses numeric literals itself instead of using the
// floating-point std::from_chars, which libc++ only exposes from macOS 26.
// These cases pin the rounding, scaling and rejection behaviour that the
// replacement has to preserve. Every port that reaches a literal is range
// checked, so magnitudes beyond the oscillator's own range are unreachable
// here; overflow and underflow are still covered through their diagnostics.
void numeric_literals(){
    require(pitch_literal("440hz")==440,"Integer literal must parse exactly.");
    require(pitch_literal("0.6hz")==0.6,"Decimal literal must round exactly, not through a reciprocal.");
    require(pitch_literal(".5hz")==0.5,"A literal may start at the decimal point.");
    require(pitch_literal("0e999hz")==0 && pitch_literal("0e-999hz")==0,"A zero mantissa must stay zero for any exponent.");
    require(pitch_literal("1e-310hz")==1e-310 && pitch_literal("1.000000000000000000e-310hz")==1e-310,"Equivalent subnormal spellings must round identically.");
    require(pitch_literal("4.9406564584124654417656879286822137236505980e-324hz")==std::numeric_limits<double>::denorm_min(),"The smallest nonzero double must be representable.");
    require(pitch_literal("1.2345678901234567890123456789hz")==1.2345678901234567890123456789,"Long decimals must preserve correct rounding.");
    require(pitch_literal("0.999999999999999944488848768742172978818416595458984375hz")==1.0,"A halfway decimal must round to even.");
    require(pitch_literal("0.999999999999999944488848768742172978818416595458984374hz")==std::nextafter(1.0,0.0),"A decimal below halfway must round down even after many significant digits.");
    require(pitch_literal("0000000000000000000000440hz")==440,"Leading zeros must not consume the kept significant digits.");
    require(pitch_literal("5e-1hz")==0.5,"Negative exponent must parse exactly.");
    require(pitch_literal("50E-2hz")==0.5,"Uppercase exponent with a sign must parse.");
    require(pitch_literal("1e3hz")==1000,"An exponent must not be mistaken for a unit suffix.");
    require(pitch_literal("0.44khz")==440,"A khz suffix scales by 1000 at parse time.");
    invalid(instrument("    osc = ClassicSine.make()\n    raw = osc(pitch: 1e999hz, level: -6db)\n    return raw * key.gate\n"),"finite");
    invalid(instrument("    osc = ClassicSine.make()\n    raw = osc(pitch: 1e-999hz, level: -6db)\n    return raw * key.gate\n"),"finite");
    invalid(instrument("    osc = ClassicSine.make()\n    raw = osc(pitch: 1e22hz, level: -6db)\n    return raw * key.gate\n"),"supported range");
}
const ParameterDescriptor& parameter_named(const Program& program,const std::string& name){
    const auto found=std::find_if(program.parameters.begin(),program.parameters.end(),[&](const auto& item){return item.name==name;});
    require(found!=program.parameters.end(),"Missing parameter '"+name+"'.");
    return *found;
}
// A class decides what each port falls back to when its argument is omitted.
// That same value is what the editor and the signal flow restore when a number
// or a knob is double-clicked, so it has to be recorded on every parameter.
void class_defaults(){
    const auto program=valid("import standard as std\nfrom lib.osc import ClassicSine\nfrom lib.env import ClassicADSR\nfrom lib.filt import ClassicFilt\n"
        "Inst.sigstream(std.event.keys.poly()) as key:\n"
        "    osc = ClassicSine.make()\n"
        "    env = ClassicADSR.make()\n"
        "    filt = ClassicFilt.make()\n"
        "    raw = osc(pitch: key.pitch, octave: 1, semitune: 3, unison: 1, level: -12db)\n"
        "    shape = env(gate: key.gate, trigger: key.trigger, A: 5ms, D: 200ms, S: 0.4, R: 400ms)\n"
        "    return filt(raw, cutoff: 1khz, resonance: 0.5) * shape\n");
    const auto default_of=[&](const std::string& name){
        const auto found=std::find_if(program.parameters.begin(),program.parameters.end(),[&](const auto& item){return item.name==name;});
        require(found!=program.parameters.end(),"Missing parameter '"+name+"'.");
        require(found->has_default,"Parameter '"+name+"' must carry the class default.");
        return found->default_value;
    };
    require(default_of("octave")==0,"octave must default to no shift.");
    require(default_of("semitune")==0,"semitune must default to no shift.");
    require(default_of("unison")==1,"unison must default to a single oscillator.");
    // The class falls back to a linear gain of 1, which is 0 dB.
    require(default_of("level")==0,"A decibel level must default to the class's 0 dB fallback.");
    require(default_of("A")==.01,"Attack must default to 10ms.");
    require(default_of("D")==.1,"Decay must default to 100ms.");
    require(default_of("S")==.6,"Sustain must default to 0.6.");
    require(default_of("R")==.2,"Release must default to 200ms.");
    require(default_of("cutoff")==800,"Cutoff must default to 800hz even when the site writes 1khz.");
    require(default_of("resonance")==.1,"Resonance must default to 0.1.");
    // A port the class requires has nothing to restore.
    const auto fixed=valid(instrument("    osc = ClassicSine.make()\n    raw = osc(pitch: 440hz, level: -6db)\n    return raw * key.gate\n"));
    const auto frequency=std::find_if(fixed.parameters.begin(),fixed.parameters.end(),[](const auto& item){return item.name=="frequency";});
    require(frequency!=fixed.parameters.end()&&!frequency->has_default,"A required port must not advertise a default.");
    // A class declaration still outranks the primitive's own fallback.
    const auto quiet=valid("import standard as std\nfrom lib.osc import ClassicSine\nclass Quiet(ClassicSine):\n    def init:\n        self.register_knob(level, default=-18db, range=(-60db, 0db), step=1db)\nInst.sigstream(std.event.keys.poly()) as key:\n    osc = Quiet.make()\n    raw = osc(pitch: key.pitch, level: -3db)\n    return raw * key.gate\n");
    const auto level=std::find_if(quiet.parameters.begin(),quiet.parameters.end(),[&](const auto& item){return item.name=="level";});
    require(level!=quiet.parameters.end(),"The subclass level parameter is missing.");
    require(level->default_value==-18&&level->has_default,"A class declaration must outrank the primitive fallback.");
    require(level->step==1 && level->min==-60 && level->max==0,"Primitive metadata must preserve the class drag step and range.");

    const auto subclass=[](const std::string& defaults,const std::string& arguments){
        return "import standard as std\nfrom lib.osc import ClassicSine\nclass Custom(ClassicSine):\n    def init:\n"+defaults+
            "Inst.sigstream(std.event.keys.poly()) as key:\n    osc = Custom.make()\n    raw = osc(pitch: key.pitch"+arguments+")\n    return raw * key.gate\n";
    };
    const auto linear=valid(subclass("        self.register_knob(level, default=-18db)\n",", level: 0.5"));
    require(std::abs(parameter_named(linear,"level").default_value-std::pow(10.0,-18.0/20.0))<1e-15,"A dB class default must reset a linear literal to the equivalent gain.");
    const auto decibels=valid(subclass("        self.register_knob(level, default=0.5)\n",", level: -3db"));
    require(parameter_named(decibels,"level").default_value==20.0*std::log10(0.5),"A linear class default must reset a dB literal to the equivalent level.");
    const auto silence=valid(subclass("        self.register_knob(level, default=0)\n",", level: -3db"));
    require(!parameter_named(silence,"level").has_default,"Zero gain cannot advertise a finite dB reset or fall through to the primitive default.");
    invalid(subclass("        self.register_knob(level, default=20)\n",", level: 0.5"),"default is outside");
    invalid(subclass("        self.register_knob(level, default=-1)\n",", level: -6db"),"default is outside");
    invalid(subclass("        self.register_knob(level, default=800hz)\n",", level: 0.5"),"incompatible units");
    invalid(subclass("        self.register_knob(unison, default=9)\n",", unison: 1"),"default is outside");

    const auto tuned=valid(subclass("        self.register_knob(octave, default=1)\n        self.register_knob(semitune, default=7)\n        self.register_knob(unison, default=1)\n",""));
    for (const auto& [name,value] : std::vector<std::pair<std::string,double>>{{"octave",1},{"semitune",7},{"unison",1}}) {
        const auto& parameter=parameter_named(tuned,name);
        require(parameter.has_default && parameter.default_value==value && tuned.nodes[parameter.value].initial_value==value,"Omitted oscillator tuning must use its registered class default.");
    }
    const auto pitch_id=tuned.nodes[named(tuned,"raw")].inputs[0];
    const auto& transposition=tuned.nodes[tuned.nodes[pitch_id].inputs[0]];
    require(transposition.op==Op::add && tuned.nodes[transposition.inputs[1]].initial_value==7,"An omitted semitune default must enter the actual oscillator signal path.");
    const auto& octave_add=tuned.nodes[transposition.inputs[0]];
    require(octave_add.op==Op::add && tuned.nodes[octave_add.inputs[1]].op==Op::multiply,"An omitted octave default must enter the actual oscillator signal path.");

    const std::string composed=
        "import standard as std\nfrom lib.osc import ClassicSine\n"
        "class Inner(ClassicSine):\n    def init:\n        self.register_knob(level, default=-12db, step=0.1db)\n"
        "class Layer:\n"
        "    def signal(pitch: pitch, gate: scalar, level: db = -6db):\n"
        "        osc = Inner.make()\n        raw = osc(pitch: pitch, level: level)\n        return raw * gate\n"
        "Inst.sigstream(std.event.keys.poly()) as key:\n    layer = Layer.make()\n"
        "    return layer(pitch: key.pitch, gate: key.gate, level: -3db)\n";
    require(parameter_named(valid(composed),"level").default_value==-6,"A composition port fallback must survive inner class and primitive defaults.");
    require(!parameter_named(valid(replace(composed,"level: db = -6db","level: db")),"level").has_default,"A required outer class port must not inherit an inner primitive reset value.");
    const auto omitted=valid(replace(composed,", level: -3db", ""));
    const auto& omitted_level=parameter_named(omitted,"level");
    require(omitted_level.default_value==-6 && omitted.nodes[omitted_level.value].initial_value==-6,"Omission and reset must agree for composition port defaults.");
    const auto outer=valid(replace(composed,"class Layer:\n", "class Layer:\n    def init:\n        self.register_knob(level, default=-18.25db, step=0.25db)\n"));
    require(parameter_named(outer,"level").default_value==-18.25 && parameter_named(outer,"level").step==.25,"Outer class defaults and explicit steps must survive inner class metadata without quantizing the default.");
    const auto filter=valid("import standard as std\nfrom lib.filt import ClassicFilt\nclass Filter(ClassicFilt):\n    def init:\n        self.register_knob(cutoff, default=900hz, range=(100hz, 10000hz), step=25hz, scale=linear)\nInst.sigstream(std.event.keys.poly()) as key:\n    filt = Filter.make()\n    return filt(key.gate, cutoff: 1000hz)\n");
    const auto& cutoff=parameter_named(filter,"cutoff");
    require(cutoff.min==100 && cutoff.max==10000 && cutoff.step==25 && !cutoff.logarithmic && cutoff.control_scale=="linear","Primitive filter metadata must retain an explicit class range, step and scale.");
}
void control_response_metadata(){
    const auto source = std::string("import standard as std\nfrom lib.osc import ClassicSine\nfrom lib.env import ClassicADSR\nfrom lib.mod import ClassicLFO\nInst.sigstream(std.event.keys.poly()) as key:\n    osc = ClassicSine.make()\n    env = ClassicADSR.make()\n    lfo = ClassicLFO.make()\n    motion = lfo(rate: 2hz)\n    raw = osc(pitch: 440hz, level: -6db)\n    amp = env(gate: key.gate, A: 1ms, D: 70ms, R: 100ms)\n    return raw * amp\n");
    const auto program=valid(source);
    const auto& attack=parameter_named(program,"A");
    require(attack.control_scale=="offset_log" && attack.control_offset==.001 && attack.step==.00001 && attack.min==0 && attack.max==30,"Envelope UI needs precise zero-compatible time response without narrowing legal values.");
    const auto& level=parameter_named(program,"level");
    require(level.control_scale=="decibels" && !level.logarithmic && level.min==-120 && level.max==24,"Level presentation must preserve existing DSP and automation domains.");
    require(parameter_named(program,"rate").control_offset==.05 && parameter_named(program,"frequency").control_offset==20,"LFO and oscillator Hz need different useful response offsets.");
    const auto gain=valid(replace(source,"level: -6db","level: 0.5"));
    require(parameter_named(gain,"level").control_scale=="gain","Linear amplitude needs a level-oriented UI response.");
}
void stereo_synthesis(){
    const std::string imports="import standard as std\nfrom lib.osc import ClassicSaw\nfrom lib.env import ClassicADSR\nfrom lib.filt import ClassicFilt\nfrom lib.mod import ClassicLFO\nfrom lib.fx import ClassicDistortion\nInst.sigstream(std.event.keys.poly()) as key:\n";
    const std::string body=
        "    osc = ClassicSaw.make()\n    lfo = ClassicLFO.make()\n    drive = ClassicDistortion.make()\n    filter = ClassicFilt.make()\n"
        "    motion = lfo(shape: triangle, rate: 2hz, mode: retrigger, trigger: key.trigger, phase: 0.25, delay: 10ms, fade: 100ms, bipolar: true)\n"
        "    wide = osc(pitch: key.pitch, unison: 3, detune: 12, spread: 0.8, pan: motion * 0.2, phase: 0.1, random: 0.5, level: -12db)\n"
        "    toned = filter(wide, cutoff: 800hz + motion * 100hz)\n"
        "    wet = drive(toned * key.gate, mode: fold, drive: 9db, mix: 0.5, output: -3db)\n"
        "    return wet\n";
    auto program=valid(imports+body);structural_invariants(program,imports+body);
    const auto& lfo=program.nodes[named(program,"motion")];
    require(lfo.op==Op::lfo&&lfo.channels==1&&lfo.inputs.size()==5&&lfo.flags==(1u|(1u<<3)|(1u<<5)),"LFO must preserve shape, retrigger, bipolar and five control inputs.");
    require(program.states[lfo.state_index].kind==StateKind::lfo,"LFO must own independent state.");
    const auto& oscillator=program.nodes[named(program,"wide")];
    require(oscillator.op==Op::unison_saw&&oscillator.flags==3&&oscillator.channels==2&&oscillator.inputs.size()==7,"Unison must lower to a stereo seven-input oscillator with structural voice count.");
    require(!parameter_named(program,"unison").live&&!parameter_named(program,"random").live,"Unison and note-start randomness require rebuild metadata.");
    for(const auto& parameter:program.parameters)if(parameter.name=="phase"&&program.nodes[parameter.value].initial_value==.1)require(!parameter.live,"Oscillator note-start phase must not claim immediate live response.");
    require(program.nodes[named(program,"toned")].channels==2,"A filter must retain both input channels.");
    const auto& distortion=program.nodes[named(program,"wet")];
    require(distortion.op==Op::distortion&&distortion.flags==2&&distortion.inputs.size()==4&&distortion.channels==2,"Distortion must preserve stereo and its shaping mode.");
    require(program.states[distortion.state_index].kind==StateKind::distortion,"Distortion ADAA needs independent per-instance memory.");
    require(program.completion_root&&program.completion[*program.completion_root].op==CompletionOp::all,"Post-envelope distortion must wait for its memory tail.");
    require(std::any_of(program.completion.begin(),program.completion.end(),[](const auto& value){return value.op==CompletionOp::distortion_settled;}),"Distortion completion witness missing.");
    require(parameter_named(program,"drive").default_value==12&&parameter_named(program,"output").default_value==0,"Distortion gains must advertise dB defaults.");
    require(program_json(program).find("\"channels\": 2")!=std::string::npos,"Channel shape must be exported in IR.");
    auto alternate=valid(replace(imports+body,"unison: 3","unison: 4"));
    require(dsp_structure_key(program)!=dsp_structure_key(alternate),"Changing unison count must change the DSP/state identity.");
    auto panned=valid(replace(imports+body,"detune: 12","detune: 15"));
    require(dsp_structure_key(program)==dsp_structure_key(panned),"Detune changes must remain live without rebuilding state.");
    auto phase=valid(replace(imports+body,"phase: 0.1","phase: 0.2"));
    require(dsp_structure_key(program)!=dsp_structure_key(phase),"Note-start phase changes must rebuild state.");

    const auto channel_source=instrument(
        "    left_signal = key.gate * 0.2\n    right_signal = key.gate * 0.8\n"
        "    pair = stereo(left_signal, right_signal)\n"
        "    l = left(pair)\n    r = right(pair)\n    m = mid(pair)\n    s = side(pair)\n"
        "    mid = m + s\n    broadcast = pair + key.gate\n"
        "    if key.pitch < C4:\n        selected = broadcast\n    else:\n        selected = l\n"
        "    return selected * 0.5 / 2\n");
    auto channels=valid(channel_source);structural_invariants(channels,channel_source);
    for(const auto& name:{"pair","broadcast","selected"})require(channels.nodes[named(channels,name)].channels==2,"Stereo shape must propagate through arithmetic and conditional broadcasting.");
    for(const auto& name:{"l","r","m","s","mid"})require(channels.nodes[named(channels,name)].channels==1,"Channel and mid/side selectors must return mono.");
    require(channels.nodes[channels.output].channels==2,"Stereo instrument output shape was lost.");
    invalid(replace(channel_source,"key.pitch < C4","pair < stereo(1, 2)"),"Comparison requires a mono");
    invalid(replace(channel_source,"key.pitch < C4","pair"),"if condition requires a mono");
    invalid(replace(channel_source,"stereo(left_signal, right_signal)","stereo(stereo(left_signal, right_signal), right_signal)"),"stereo left input requires a mono");
    invalid(replace(imports+body,"pan: motion * 0.2","pan: stereo(motion, motion)"),"pan requires a mono");
    invalid(replace(imports+body,"unison: 3","unison: 1.5"),"constant integer");
    invalid(replace(imports+body,"unison: 3","unison: key.gate"),"constant integer");
    invalid(replace(imports+body,"random: 0.5","random: key.gate"),"constant note-start");
    invalid(replace(imports+body,"drive: 9db","drive: stereo(1, 1)"),"drive requires a mono");
    invalid(replace(imports+body,"mode: fold","mode: unknown"),"Distortion mode");
    invalid(replace(imports+body,"rate: 2hz","rate: 2hz, sync: 1/4"),"mutually exclusive");
    invalid(replace(imports+body,"rate: 2hz","rate: 2"),"requires hertz");
    invalid(replace(imports+body,"shape: triangle","shape: unknown"),"LFO shape");
    invalid(replace(imports+body,"mode: retrigger, trigger: key.trigger","mode: retrigger"),"explicit trigger");
    invalid(replace(imports+body,"mode: retrigger","mode: transport"),"requires sync");
    auto synchronized=valid(replace(replace(imports+body,"rate: 2hz","sync: 1/4"),"mode: retrigger","mode: transport"));
    require(synchronized.nodes[named(synchronized,"motion")].flags==(1u|(3u<<3)|(1u<<5)|(1u<<6)),"Transport LFO must encode sync and transport mode without changing its input count.");
    auto one_shot=valid(replace(replace(imports+body,"shape: triangle","shape: sample_hold"),"mode: retrigger","mode: one_shot"));
    require(one_shot.nodes[named(one_shot,"motion")].flags==(4u|(2u<<3)|(1u<<5)),"Sample/hold one-shot LFO flags missing.");
    const auto selected_gate=valid(instrument("    pair = stereo(key.gate, 0.5)\n    return left(pair)\n"));
    require(selected_gate.completion_root.has_value(),"Selecting a gated stereo channel must preserve that channel's completion proof.");
    invalid(instrument("    pair = stereo(key.gate, 0.5)\n    return right(pair)\n"),"lifetime is unproven");
    auto custom_shape=valid("import standard as std\nclass Controls:\n    def signal(shape: scalar, mode: scalar):\n        return shape * mode\nInst.sigstream(std.event.keys.poly()) as key:\n    controls = Controls.make()\n    return controls(shape: 0.5, mode: key.gate)\n");
    require(custom_shape.completion_root.has_value(),"Symbolic builtin argument names must remain usable as typed composition ports.");
    auto fresh_sine=valid(instrument("    osc = ClassicSine.make()\n    raw = osc(pitch: key.pitch, pan: 0)\n    return raw * key.gate\n"));
    require(fresh_sine.nodes[named(fresh_sine,"raw")].op==Op::unison_sine&&fresh_sine.nodes[named(fresh_sine,"raw")].flags==1,"Explicit stereo controls must activate extended oscillation even for one voice.");
}

const ChoiceDescriptor& choice_named(const Program& program,const std::string& owner,const std::string& name){
    const auto found=std::find_if(program.choices.begin(),program.choices.end(),[&](const auto& choice){return choice.owner==owner&&choice.name==name;});
    require(found!=program.choices.end(),"Missing choice "+owner+"."+name);return *found;
}
const ChoiceOption& option_named(const ChoiceDescriptor& choice,const std::string& value){
    const auto found=std::find_if(choice.options.begin(),choice.options.end(),[&](const auto& option){return option.value==value;});
    require(found!=choice.options.end(),"Missing choice option "+value);return *found;
}
void all_enabled_choices_compile(const std::string& source){
    const auto program=valid(source);std::set<std::string> bindings;
    for(const auto& choice:program.choices){
        require(bindings.insert(choice.binding_id).second,"Choice bindings must be unique even for shared class spans.");
        require(choice.span.offset<=source.size()&&choice.span.length<=source.size()-choice.span.offset,"Choice span exceeds source.");
        require(choice.implicit?choice.span.length==0:source.substr(choice.span.offset,choice.span.length)==choice.selected,"A choice must target its complete explicit value or a zero-width insertion point.");
        if(!choice.owner.empty())require(std::any_of(program.named_values.begin(),program.named_values.end(),[&](const auto& value){return value.name==choice.owner;}),"Choice owner must be an actual named flow signal: "+choice.owner);
        option_named(choice,choice.selected);
        for(const auto& option:choice.options){
            if(!option.enabled){require(!option.reason.empty(),"A disabled option must explain its prerequisite.");continue;}
            auto replacement=source;replacement.replace(choice.span.offset,choice.span.length,choice.prefix+option.value+choice.suffix);
            const auto result=compile(replacement,"choice-replacement.flows");
            require(result.program.has_value(),"Enabled choice "+choice.binding_id+" -> "+option.value+" failed: "+(result.diagnostics.empty()?std::string{}:result.diagnostics.front().message));
        }
    }
    auto metadata=program;metadata.choices.clear();
    require(dsp_structure_key(program)==dsp_structure_key(metadata),"Choice presentation metadata must not participate in DSP identity.");
    require(program_json(program).find("\"choices\"")!=std::string::npos,"Choice metadata must be exported in IR.");
}
void discrete_choices(){
    const auto root=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    for(const auto& file:std::filesystem::directory_iterator(root/"test/presets"))if(file.path().extension()==".flows")all_enabled_choices_compile(read(file.path()));
    const std::string source=
        "import standard as std\nfrom lib.osc import ClassicSaw\nfrom lib.env import ClassicADSR\nfrom lib.filt import ClassicFilt\nfrom lib.mod import ClassicLFO\nfrom lib.fx import ClassicDistortion\n"
        "Inst.sigstream(std.event.keys.mono()) as key:\n"
        "    osc = ClassicSaw.make()\n    lfo = ClassicLFO.make()\n    filter = ClassicFilt.make()\n    env = ClassicADSR.make()\n    drive = ClassicDistortion.make()\n"
        "    motion = lfo()\n    raw = osc(pitch: key.pitch)\n"
        "    filtered = filter(\n        raw, # input stays a positional argument\n        cutoff: 800hz, # preserve trailing comma and comment\n    )\n"
        "    amp = env(\n        gate: key.gate # final argument has no comma\n    )\n"
        "    shaped = drive(filtered, mode: softclip)\n    return shaped * amp\n";
    all_enabled_choices_compile(source);
    const auto program=valid(source);
    for(const auto& [owner,name]:std::vector<std::pair<std::string,std::string>>{{"motion","shape"},{"motion","mode"},{"motion","bipolar"},{"filtered","mode"},{"amp","retrigger_from"},{"key.pitch","priority"},{"key.pitch","retrigger"}})
        require(choice_named(program,owner,name).implicit,"Omitted builtin choices must be exposed for Signal Flow.");
    const auto& lfo_mode=choice_named(program,"motion","mode");
    require(!option_named(lfo_mode,"retrigger").enabled&&!option_named(lfo_mode,"one_shot").enabled&&!option_named(lfo_mode,"transport").enabled,"LFO dropdown must not invent trigger or sync signals.");
    require(!choice_named(program,"shaped","mode").implicit,"Explicit distortion mode must be replaceable in the editor.");
    require(option_named(choice_named(program,"key.pitch","mode"),"poly").enabled,"A mono() instrument with a proven release may switch to poly().");
    const auto kwargs=valid(replace(source,"keys.mono()","keys.mono(priority: last, retrigger: gate_rise, glide: 20ms)"));
    require(!option_named(choice_named(kwargs,"key.pitch","mode"),"poly").enabled,"Poly choice must not silently discard mono arguments.");
    all_enabled_choices_compile(replace(source,"keys.mono()","keys.mono(priority: last, retrigger: gate_rise, glide: 20ms)"));
    const auto unproven=valid(replace(source,"return shaped * amp","return shaped"));
    require(!option_named(choice_named(unproven,"key.pitch","mode"),"poly").enabled,"Poly choice must preserve the output lifetime requirement.");
    all_enabled_choices_compile(replace(source,"return shaped * amp","return shaped"));
    const auto synced=replace(source,"motion = lfo()","motion = lfo(sync: 1/4, trigger: key.trigger, bipolar: false)");
    const auto synchronized=valid(synced);
    require(std::all_of(choice_named(synchronized,"motion","mode").options.begin(),choice_named(synchronized,"motion","mode").options.end(),[](const auto& option){return option.enabled;}),"An LFO with sync and trigger must expose every mode.");
    require(!choice_named(synchronized,"motion","bipolar").implicit,"A true/false literal must have an exact choice span.");
    all_enabled_choices_compile(synced);
    for(const auto& expression:{"flag","1","(true)","1 - 1"}){
        const auto text=replace(source,"    motion = lfo()","    flag = true\n    motion = lfo(bipolar: "+std::string(expression)+")");
        const auto compiled=valid(text);
        require(std::none_of(compiled.choices.begin(),compiled.choices.end(),[](const auto& choice){return choice.owner=="motion"&&choice.name=="bipolar";}),"A bipolar dropdown must not replace a variable or expression.");
        all_enabled_choices_compile(text);
    }
    const auto aliased=replace(replace(source," as key:"," as note:"),"pitch: key.pitch","pitch: note.pitch");
    auto aliased_gate=replace(aliased,"gate: key.gate","gate: note.gate");
    require(choice_named(valid(aliased_gate),"note.pitch","mode").selected=="mono","Key policy choices must follow the actual key alias.");
    all_enabled_choices_compile(aliased_gate);
    const auto event_variable=replace(source,"Inst.sigstream(std.event.keys.mono()) as key:","events = std.event.keys.mono(\n    # no authored options\n)\nInst.sigstream(events) as key:");
    all_enabled_choices_compile(event_variable);

    const std::string nested=
        "import standard as std\nfrom lib.filt import ClassicFilt\n"
        "class Tone:\n    def signal(input: audio):\n        filter = ClassicFilt.make()\n        filtered = filter(input, mode: lowpass)\n        return filtered\n"
        "class Layer:\n    def signal(input: audio):\n        tone = Tone.make()\n        return tone(input: input)\n"
        "Inst.sigstream(std.event.keys.poly()) as key:\n    a = Layer.make()\n    b = Layer.make()\n    one = a(input: key.gate)\n    two = b(input: key.gate)\n    return one + two\n";
    const auto shared=valid(nested);
    const auto& first=choice_named(shared,"a.tone.filtered","mode");const auto& second=choice_named(shared,"b.tone.filtered","mode");
    require(first.span.offset==second.span.offset&&first.binding_id!=second.binding_id,"Shared class text needs one exact span and distinct owner bindings.");
    all_enabled_choices_compile(nested);
    const auto returns=replace(nested,"        filtered = filter(input, mode: lowpass)\n        return filtered","        return filter(input)");
    const auto returned=valid(returns);
    require(choice_named(returned,"one","mode").implicit&&choice_named(returned,"two","mode").implicit,"Nested class return choices must map to the outer named call result.");
    all_enabled_choices_compile(returns);
    const auto direct_return="import standard as std\nfrom lib.filt import ClassicFilt\nInst.sigstream(std.event.keys.poly()) as key:\n    filter = ClassicFilt.make()\n    return filter(key.gate, mode: highpass)\n";
    require(choice_named(valid(direct_return),"$output","mode").selected=="highpass","Top-level return choices must belong to $output.");
    all_enabled_choices_compile(direct_return);
    const std::string metadata=
        "import standard as std\nfrom lib.mod import ClassicLFO\n"
        "class Motion(ClassicLFO):\n    def init:\n        self.register_knob(rate, scale=linear, range=(0hz, 10hz), update=live)\n"
        "        self.register_knob(bipolar, default=1, range=(0, 1))\n"
        "Inst.sigstream(std.event.keys.poly()) as key:\n    lfo = Motion.make()\n    motion = lfo()\n    return motion * key.gate\n";
    const auto registered=valid(metadata);
    require(!option_named(choice_named(registered,"","scale"),"log").enabled,"Log scale requires a positive registered range.");
    require(std::none_of(choice_named(registered,"motion","bipolar").options.begin(),choice_named(registered,"motion","bipolar").options.end(),[](const auto& option){return option.enabled;}),"Boolean insertion must respect class metadata units.");
    all_enabled_choices_compile(metadata);
    const auto positive=replace(metadata,"(0hz, 10hz)","(0.1hz, 10hz)");
    require(option_named(choice_named(valid(positive),"","scale"),"log").enabled,"Positive registered ranges must expose logarithmic scale.");
    all_enabled_choices_compile(positive);
}

}
int main(){try{sine_regression();repository_examples();conditional_and_completion();observations_and_identity();classes_and_metadata();resource_budgets();diagnostics();numeric_literals();class_defaults();control_response_metadata();stereo_synthesis();discrete_choices();std::cout<<"Compiler tests passed: 5 public instruments, 3 internal fixtures, matching documentation, typed classes, metadata, observations, identity, diagnostics, numeric literals and class defaults.\n";return 0;}catch(const std::exception& error){std::cerr<<"Compiler test failure: "<<error.what()<<'\n';return 1;}}
