#include "compiler/program.hpp"
#include "engine/engine.hpp"
#include "engine/sample.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

double number(const std::string& text) {
    std::size_t consumed = 0;
    const auto value = std::stod(text, &consumed);
    if (consumed != text.size() || !std::isfinite(value))
        throw std::runtime_error("Expected a finite number, got: " + text);
    return value;
}

std::uint32_t integer(const std::string& text, std::uint32_t low, std::uint32_t high) {
    const auto value = number(text);
    if (value < low || value > high || value != std::floor(value))
        throw std::runtime_error("Integer outside supported range: " + text);
    return static_cast<std::uint32_t>(value);
}

using Options = std::map<std::string, std::string>;

Options options(int argc, char** argv, const std::set<std::string>& allowed) {
    Options result;
    for (int i = 3; i < argc; i += 2) {
        const std::string key(argv[i]);
        if (!allowed.contains(key) || i + 1 >= argc || result.contains(key))
            throw std::runtime_error("Unknown, repeated or incomplete option: " + key);
        result[key] = argv[i + 1];
    }
    return result;
}

std::string option(const Options& opts, const std::string& key, std::string fallback = {}) {
    const auto found = opts.find(key);
    return found == opts.end() ? fallback : found->second;
}

std::string required(const Options& opts, const std::string& key) {
    const auto value = option(opts, key);
    if (value.empty()) throw std::runtime_error("Missing " + key);
    return value;
}

void write_text(const fs::path& path, const std::string& text) {
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << text;
    out.close();
}

void u16(std::ostream& out, std::uint16_t value) {
    out.put(static_cast<char>(value & 255));
    out.put(static_cast<char>((value >> 8) & 255));
}

void u32(std::ostream& out, std::uint32_t value) {
    u16(out, static_cast<std::uint16_t>(value & 65535));
    u16(out, static_cast<std::uint16_t>(value >> 16));
}

void wav_header(std::ostream& out, std::uint32_t rate, std::uint32_t frames, std::uint32_t channels) {
    out.write("RIFF", 4); u32(out, 36 + frames * 2 * channels); out.write("WAVEfmt ", 8);
    u32(out, 16); u16(out, 1); u16(out, static_cast<std::uint16_t>(channels)); u32(out, rate);
    u32(out, rate * 2 * channels); u16(out, static_cast<std::uint16_t>(2 * channels)); u16(out, 16);
    out.write("data", 4); u32(out, frames * 2 * channels);
}

std::vector<flows::Event> read_events(const fs::path& path, std::uint64_t frames) {
    std::istringstream input(read_file(path));
    std::vector<flows::Event> events;
    std::map<std::uint64_t, double> held;
    std::set<std::uint64_t> seen;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty() || line.front() == '#') continue;
        std::istringstream row(line);
        flows::Event event;
        std::string kind, sample, id, pitch, velocity, extra;
        if (!(row >> sample >> kind >> id >> pitch >> velocity) || (row >> extra))
            throw std::runtime_error("Bad event row " + std::to_string(line_number));
        // The offline fixture adapter uses numeric identities; no MIDI identity inference.
        const auto sample_value = number(sample);
        const auto id_value = number(id);
        if (sample_value < 0 || sample_value >= static_cast<double>(frames)
            || sample_value != std::floor(sample_value) || id_value < 0
            || id_value > 4294967295.0 || id_value != std::floor(id_value))
            throw std::runtime_error("Invalid event sample or identity on row " + std::to_string(line_number));
        event.sample = static_cast<std::uint64_t>(sample_value);
        event.note_id = static_cast<std::uint64_t>(id_value);
        event.pitch = number(pitch);
        event.velocity = number(velocity);
        if (event.pitch < 0 || event.pitch > 127 || event.velocity < 0 || event.velocity > 1)
            throw std::runtime_error("Event pitch / velocity outside domain");
        if (!events.empty() && event.sample < events.back().sample)
            throw std::runtime_error("Events must be sorted by sample");
        if (kind == "note_on") {
            event.type = flows::EventType::note_on;
            if (!seen.insert(event.note_id).second) throw std::runtime_error("Duplicate note identity");
            held.emplace(event.note_id, event.pitch);
        } else if (kind == "note_off") {
            event.type = flows::EventType::note_off;
            const auto note = held.find(event.note_id);
            if (note == held.end() || note->second != event.pitch)
                throw std::runtime_error("Note-off does not match a held note");
            held.erase(note);
        } else throw std::runtime_error("Unsupported event type: " + kind);
        events.push_back(event);
    }
    if (!held.empty()) throw std::runtime_error("Offline event fixture must release all notes");
    return events;
}

// Capture is bounded during Engine::process. CSV/file operations happen afterwards.
class BlockCapture final : public flows::CaptureSink {
    struct Chunk {
        flows::ValueId value;
        flows::VoiceIdentity voice;
        std::uint64_t start;
        std::size_t offset;
        std::size_t count;
    };
    struct EventRecord {
        flows::Event event;
        flows::VoiceIdentity voice;
        std::string_view action;
    };
    std::vector<bool> selected_, audio_;
    std::string projection_;
    std::vector<float> samples_;
    std::vector<Chunk> chunks_;
    std::vector<EventRecord> events_;
    std::size_t used_ = 0;
    bool overflow_ = false;

public:
    BlockCapture(const flows::Program& program, const std::string& names, std::uint32_t block, std::string projection)
        : selected_(program.nodes.size(), false), audio_(program.nodes.size()), projection_(std::move(projection)) {
        for (flows::ValueId id=0; id<program.nodes.size(); ++id) {
            const auto& node=program.nodes[id];
            audio_[id]=node.op==flows::Op::phasor || node.op==flows::Op::sampler || node.op==flows::Op::sine || node.op==flows::Op::saw || node.op==flows::Op::unison_sine || node.op==flows::Op::unison_saw || node.op==flows::Op::filter_lowpass || node.op==flows::Op::filter_highpass;
            if (node.op!=flows::Op::lfo) for (auto input:node.inputs) audio_[id]=audio_[id] || audio_[input];
        }
        std::istringstream stream(names);
        std::string name;
        std::size_t count = 0;
        while (std::getline(stream, name, ',')) {
            if (name.empty()) continue;
            const auto binding = std::find_if(program.named_values.begin(), program.named_values.end(),
                [&](const auto& item) { return item.name == name; });
            if (binding == program.named_values.end())
                throw std::runtime_error("Unknown observation: " + name);
            if (!selected_[binding->value]) { selected_[binding->value] = true; ++count; }
        }
        if (count > 64) throw std::runtime_error("Offline capture supports up to 64 observed values per render");
        samples_.resize(32 * count * block);
        chunks_.reserve(32 * count * (std::min(block, 256U) + 1));
        events_.reserve(512);
    }

    void begin() noexcept { used_ = 0; overflow_ = false; chunks_.clear(); events_.clear(); }

    void capture(flows::ValueId value, flows::VoiceIdentity voice, std::uint64_t start,
                 std::span<const float> samples) noexcept override { store(value, voice, start, samples, {}); }
    void capture_stereo(flows::ValueId value, flows::VoiceIdentity voice, std::uint64_t start,
                        std::span<const float> left, std::span<const float> right) noexcept override { store(value, voice, start, left, right); }
    void store(flows::ValueId value, flows::VoiceIdentity voice, std::uint64_t start,
               std::span<const float> samples, std::span<const float> right) noexcept {
        if (value >= selected_.size() || !selected_[value]) return;
        if (used_ + samples.size() > samples_.size() || chunks_.size() == chunks_.capacity()) {
            overflow_ = true; return;
        }
        for (std::size_t i=0; i<samples.size(); ++i) {
            const float l=samples[i], r=right.empty()?l:right[i];
            samples_[used_+i]=!audio_[value] ? l : projection_=="left" ? l : projection_=="right" ? r : projection_=="side" ? (l-r)*.5f : right.empty()?l:(l+r)*.5f;
        }
        chunks_.push_back({value, voice, start, used_, samples.size()});
        used_ += samples.size();
    }

    void event(const flows::Event& event, flows::VoiceIdentity voice, std::string_view action) noexcept override {
        if (events_.size() == events_.capacity()) { overflow_ = true; return; }
        events_.push_back({event, voice, action});
    }

    void flush(std::ostream& captures, std::ostream& events) const {
        if (overflow_) throw std::runtime_error("Offline capture capacity exceeded; no complete result published");
        for (const auto& chunk : chunks_)
            for (std::size_t i = 0; i < chunk.count; ++i)
                captures << chunk.start + i << ',' << chunk.value << ',' << chunk.voice.slot << ','
                         << chunk.voice.generation << ',' << chunk.voice.note_id << ','
                         << samples_[chunk.offset + i] << '\n';
        for (const auto& item : events_)
            events << item.event.sample << ',' << item.event.note_id << ',' << item.voice.slot << ','
                   << item.voice.generation << ',' << item.action << '\n';
    }
};

int render(const flows::Program& program, const Options& opts) {
    const auto rate = integer(option(opts, "--sample-rate", "48000"), 8000, 192000);
    const auto block = integer(option(opts, "--block-size", "64"), 1, 4096);
    const auto duration = number(option(opts, "--duration", "1"));
    if (duration <= 0 || duration > 60) throw std::runtime_error("Duration must be in (0, 60] seconds");
    const auto frames = static_cast<std::uint32_t>(std::floor(duration * rate + 0.5));
    if (frames == 0) throw std::runtime_error("Duration is shorter than one sample");
    const auto events = read_events(required(opts, "--events"), frames);
    flows::Engine engine(program, {static_cast<double>(rate), block, 32}, flows::load_sample_bank(program, option(opts, "--asset-root")));
    const auto channels=program.nodes[program.output].channels;
    const double bpm=number(option(opts,"--bpm","138"));
    if (bpm<20 || bpm>300) throw std::runtime_error("BPM must be 20..300");
    const auto projection=option(opts,"--capture-channel","mid");
    if (projection!="mid" && projection!="left" && projection!="right" && projection!="side") throw std::runtime_error("Capture channel must be mid, left, right or side");
    engine.set_transport({bpm,0,true,true});
    BlockCapture capture(program, option(opts, "--observe"), block, projection);
    const fs::path output(required(opts, "--output"));
    const std::vector<std::string> artifacts{
        "output.wav", "captures.csv", "events.csv", "program.ir.json", "engine.json"};
    for (const auto& name : artifacts)
        if (fs::exists(output / name))
            throw std::runtime_error("Output already contains published artifacts; choose a new directory");
    fs::create_directories(output);
    std::ofstream wav(output / "output.wav.partial", std::ios::binary);
    std::ofstream captures(output / "captures.csv.partial");
    std::ofstream lifecycle(output / "events.csv.partial");
    for (auto* file : {&wav, &captures, &lifecycle}) file->exceptions(std::ios::badbit | std::ios::failbit);
    wav_header(wav, rate, frames, channels);
    captures << std::setprecision(9) << "sample,value_id,voice_slot,voice_generation,note_id,value\n";
    lifecycle << "sample,note_id,voice_slot,voice_generation,action\n";
    std::vector<float> audio(block), right(block);
    std::size_t next_event = 0;
    std::uint64_t clipped = 0;
    double peak = 0;
    for (std::uint64_t start = 0; start < frames; start += block) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(block, frames - start));
        const auto begin = next_event;
        while (next_event < events.size() && events[next_event].sample < start + count) ++next_event;
        capture.begin();
        const auto processed = channels==2 ? engine.process_stereo(std::span(events).subspan(begin, next_event - begin), std::span(audio).first(count), std::span(right).first(count), &capture) : engine.process(std::span(events).subspan(begin, next_event - begin), std::span(audio).first(count), &capture);
        if (!processed) throw std::runtime_error("Engine rejected block: " + std::to_string(static_cast<int>(processed.error)));
        // Offline driver may write files; the engine callback above does not.
        capture.flush(captures, lifecycle);
        for (std::size_t i = 0; i < count; ++i) for (std::uint32_t channel=0; channel<channels; ++channel) {
            const double value = channel==0 ? audio[i] : right[i];
            if (!std::isfinite(value)) throw std::runtime_error("DSP produced a non-finite output");
            peak = std::max(peak, std::abs(value));
            if (std::abs(value) > 1.0) ++clipped;
            const auto pcm = static_cast<std::int16_t>(std::lround(std::clamp(value, -1.0, 1.0) * 32767.0));
            u16(wav, static_cast<std::uint16_t>(pcm));
        }
    }
    wav.close(); captures.close(); lifecycle.close();
    const auto& stats = engine.stats();
    std::ostringstream report;
    report << std::setprecision(12)
           << "{\n  \"schema_version\": 1,\n  \"status\": \"rendered\",\n"
           << "  \"backend\": \"native-block-v1\",\n"
           << "  \"sample_rate\": " << rate << ",\n  \"block_size\": " << block
           << ",\n  \"channels\": " << channels << ",\n  \"bpm\": " << bpm << ",\n  \"capture_projection\": \"" << projection << "\",\n  \"duration_seconds\": " << duration
           << ",\n  \"rendered_frames\": " << frames
           << ",\n  \"accepted_notes\": " << stats.accepted_notes
           << ",\n  \"rejected_notes\": " << stats.rejected_notes
           << ",\n  \"completed_notes\": " << stats.completed_notes
           << ",\n  \"active_voices\": " << stats.active_voices
           << ",\n  \"clipped_samples\": " << clipped
           << ",\n  \"peak_before_quantization\": " << peak
           << ",\n  \"wav_encoding\": \"PCM16 " << (channels==2?"stereo":"mono") << "\"\n}\n";
    write_text(output / "engine.json.partial", report.str());
    write_text(output / "program.ir.json.partial", flows::program_json(program));
    // Publish the completion marker last. Never mix these artifacts with an older render.
    for (const auto& name : artifacts)
        fs::rename(output / (name + ".partial"), output / name);
    std::cout << "Rendered " << frames << " samples at " << rate << " Hz to " << (output / "output.wav")
              << "\nAccepted notes: " << stats.accepted_notes << ", completed: " << stats.completed_notes
              << ", clipped samples: " << clipped << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3) {
            std::cerr << "Usage: flows compile SOURCE [--ir PATH --asset-root DIR]\n"
                         "       flows render SOURCE --events FILE.tsv --output DIR "
                         "[--sample-rate 48000 --block-size 64 --duration 1 --observe raw,key.pitch --bpm 138 --capture-channel mid --asset-root DIR]\n";
            return 2;
        }
        const std::string command(argv[1]);
        if (command != "compile" && command != "render") throw std::runtime_error("Unknown command: " + command);
        const auto opts = options(argc, argv, command == "compile"
            ? std::set<std::string>{"--ir", "--asset-root"}
            : std::set<std::string>{"--events", "--output", "--sample-rate", "--block-size", "--duration", "--observe", "--bpm", "--capture-channel", "--asset-root"});
        const auto source = read_file(argv[2]);
        const auto compiled = flows::compile(source, fs::absolute(argv[2]).string());
        for (const auto& diagnostic : compiled.diagnostics)
            std::cerr << argv[2] << ':' << diagnostic.span.line << ':' << diagnostic.span.column
                      << ": error: " << diagnostic.message << '\n';
        if (!compiled.program) return 1;
        if (command == "render") return render(*compiled.program, opts);
        // Compilation validates that referenced resources can be prepared too.
        (void)flows::load_sample_bank(*compiled.program, option(opts, "--asset-root"));
        const auto json = flows::program_json(*compiled.program);
        const auto path = option(opts, "--ir");
        if (path.empty()) std::cout << json;
        else { write_text(path, json); std::cout << "Compiled " << compiled.program->nodes.size() << " operations to " << path << '\n'; }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "flows: " << error.what() << '\n';
        return 1;
    }
}
