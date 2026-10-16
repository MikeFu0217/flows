// Standalone microbenchmark, deliberately excluding GUI, driver and host scheduling.
// Build with the same release optimization flags as the engine; no fast-math.
#include "compiler/program.hpp"
#include "engine/engine.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <new>
#include <sstream>
#include <vector>

namespace { bool observing = false; std::size_t allocations = 0, deallocations = 0; }
void* operator new(std::size_t size) {
    if (observing) ++allocations;
    if (void* memory = std::malloc(std::max<std::size_t>(1, size))) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { if (observing && memory) ++deallocations; std::free(memory); }
void operator delete[](void* memory) noexcept { ::operator delete(memory); }
void operator delete(void* memory, std::size_t) noexcept { ::operator delete(memory); }
void operator delete[](void* memory, std::size_t) noexcept { ::operator delete(memory); }

namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) { return std::chrono::duration<double, std::micro>(Clock::now() - start).count(); }
struct Capture final : flows::CaptureSink {
    std::array<flows::ValueId, 8> ids{};
    std::array<std::size_t, 8> channel_offsets{};
    std::size_t count = 0, channels = 0, block = 0;
    std::uint64_t start = 0, bytes = 0;
    std::vector<float> values;
    Capture(const flows::Program& p, std::size_t length) : block(length) {
        for (const auto& value : p.named_values) {
            if (std::find(ids.begin(), ids.begin() + count, value.value) != ids.begin() + count) continue;
            channel_offsets[count] = channels;
            channels += p.nodes[value.value].channels;
            ids[count++] = value.value;
            if (count == ids.size()) break;
        }
        values.resize(channels * block);
    }
    void begin(std::uint64_t sample) noexcept { start = sample; std::fill(values.begin(), values.end(), 0.0f); }
    void event(const flows::Event&, flows::VoiceIdentity, std::string_view) noexcept override {}
    void capture(flows::ValueId value, flows::VoiceIdentity, std::uint64_t sample,
                 std::span<const float> samples) noexcept override {
        for (std::size_t signal = 0; signal < count; ++signal) if (ids[signal] == value)
            capture_channel(channel_offsets[signal], sample, samples);
    }
    void capture_stereo(flows::ValueId value, flows::VoiceIdentity, std::uint64_t sample,
                        std::span<const float> left, std::span<const float> right) noexcept override {
        for (std::size_t signal = 0; signal < count; ++signal) if (ids[signal] == value) {
            capture_channel(channel_offsets[signal], sample, left);
            capture_channel(channel_offsets[signal] + 1, sample, right);
        }
    }
    void capture_channel(std::size_t channel, std::uint64_t sample, std::span<const float> samples) noexcept {
        const auto offset = sample - start;
        if (channel >= channels || offset > block || samples.size() > block - offset) std::abort();
        for (std::size_t i = 0; i < samples.size(); ++i) values[channel * block + offset + i] += samples[i];
        bytes += samples.size_bytes();
    }
};
}

int main(int argc, char** argv) {
    if (argc != 9 && argc != 10) {
        std::cerr << "Usage: engine_benchmark source.flows sample_rate block_size voices warmup_seconds measured_seconds capture_0_or_1 events.tsv_or_dash [boundary_seconds]\n";
        return 2;
    }
    try {
        const double rate = std::stod(argv[2]), warmup = std::stod(argv[5]), duration = std::stod(argv[6]);
        const auto block = static_cast<std::uint32_t>(std::stoul(argv[3]));
        const auto voices = static_cast<std::uint32_t>(std::stoul(argv[4]));
        const bool capture = std::stoi(argv[7]) != 0;
        if (voices < 1 || voices > 32 || !std::isfinite(rate) || !std::isfinite(warmup) || !std::isfinite(duration) || warmup < 0 || duration <= 0 || duration > 600) return 2;
        std::ifstream sourceFile(argv[1]);
        if (!sourceFile) return 2;
        std::string source((std::istreambuf_iterator<char>(sourceFile)), {});
        const auto compileStart = Clock::now();
        auto result = flows::compile(source, argv[1]);
        const auto compileUs = elapsed(compileStart);
        if (!result.program) { std::cerr << "Compilation failed\n"; return 2; }
        const double boundarySeconds = argc == 10 ? std::stod(argv[9]) : 0;
        flows::Engine engine(*result.program, {rate, block, 32, boundarySeconds});
        Capture sink(*result.program, block);
        if (capture) engine.set_capture_values(std::span(sink.ids).first(sink.count));
        const auto outputChannels = result.program->nodes[result.program->output].channels;
        std::vector<flows::Event> events;
        if (std::string_view(argv[8]) == "-") {
            for (std::uint32_t i = 0; i < voices; ++i)
                events.push_back({0, flows::EventType::note_on, i + 1, double(48 + i), 0.1});
        } else {
            std::ifstream eventFile(argv[8]);
            flows::Event event;
            std::string kind;
            while (eventFile >> event.sample >> kind >> event.note_id >> event.pitch >> event.velocity) {
                if (kind != "note_on" && kind != "note_off") return 2;
                event.type = kind == "note_on" ? flows::EventType::note_on : flows::EventType::note_off;
                events.push_back(event);
            }
            if (!eventFile.eof()) return 2;
        }
        const auto warmupFrames = static_cast<std::uint64_t>(std::llround(warmup * rate));
        const auto end = warmupFrames + static_cast<std::uint64_t>(std::llround(duration * rate));
        std::vector<float> output(block), right(outputChannels == 2 ? block : 0);
        std::vector<double> timings;
        timings.reserve(static_cast<std::size_t>((end - warmupFrames + block - 1) / block));
        std::size_t next = 0;
        std::uint64_t captureBytes = 0, misses = 0, outputHash = 14695981039346656037ULL;
        double checksum = 0, checksumLeft = 0, checksumRight = 0, maxFraction = 0;
        for (std::uint64_t sample = 0; sample < end;) {
            const auto boundary = sample < warmupFrames ? warmupFrames : end;
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(block, boundary - sample));
            auto first = next;
            while (next < events.size() && events[next].sample < sample + count) ++next;
            observing = true;
            const auto start = Clock::now();
            if (capture) sink.begin(sample);
            const auto inputEvents = std::span(events).subspan(first, next - first);
            const auto ok = outputChannels == 2
                ? engine.process_stereo(inputEvents, std::span(output).first(count), std::span(right).first(count), capture ? &sink : nullptr)
                : engine.process(inputEvents, std::span(output).first(count), capture ? &sink : nullptr);
            const auto microseconds = elapsed(start);
            observing = false;
            if (!ok) { std::cerr << "Engine process failed\n"; return 1; }
            if (sample >= warmupFrames) {
                timings.push_back(microseconds);
                const auto fraction = microseconds * 1e-6 / (double(count) / rate);
                maxFraction = std::max(maxFraction, fraction);
                if (fraction > 1) ++misses;
                checksumLeft += output[count / 2];
                if (outputChannels == 2) checksumRight += right[count / 2];
                checksum += output[count / 2] + (outputChannels == 2 ? 2.0 * right[count / 2] : 0.0);
                // Outside the timed interval: verify every sample in every output channel.
                for (std::size_t i = 0; i < count; ++i) {
                    outputHash = (outputHash ^ std::bit_cast<std::uint32_t>(output[i])) * 1099511628211ULL;
                    if (outputChannels == 2) outputHash = (outputHash ^ std::bit_cast<std::uint32_t>(right[i])) * 1099511628211ULL;
                }
            } else captureBytes = sink.bytes;
            sample += count;
        }
        std::sort(timings.begin(), timings.end());
        const auto quantile = [&](double p) { return timings[static_cast<std::size_t>(std::floor((timings.size() - 1) * p))]; };
        const auto& stats = engine.stats();
        std::cout << std::setprecision(12) << "{\"compile_time_ms\":" << compileUs / 1000
            << ",\"render_time_p50_us\":" << quantile(0.5) << ",\"render_time_p99_us\":" << quantile(0.99)
            << ",\"render_time_max_us\":" << timings.back() << ",\"deadline_misses\":" << misses
            << ",\"render_time_max_deadline_fraction\":" << maxFraction
            << ",\"boundary_seconds\":" << boundarySeconds
            << ",\"rt_allocations\":" << allocations << ",\"rt_deallocations\":" << deallocations
            << ",\"probe_drops\":0,\"probe_capture_bytes\":" << sink.bytes - captureBytes
            << ",\"capture_signals\":" << (capture ? sink.count : 0)
            << ",\"capture_channels\":" << (capture ? sink.channels : 0)
            << ",\"channel_count\":" << outputChannels
            << ",\"voice_instance_count\":" << stats.voice_instance_count << ",\"sounding_voice_count\":" << stats.active_voices
            << ",\"blocks\":" << timings.size() << ",\"checksum\":" << checksum
            << ",\"checksum_left\":" << checksumLeft << ",\"checksum_right\":" << checksumRight
            << ",\"output_hash\":\"" << std::hex << outputHash << "\"}\n";
        return allocations || deallocations ? 1 : 0;
    } catch (const std::exception& error) { observing = false; std::cerr << error.what() << '\n'; return 2; }
}
