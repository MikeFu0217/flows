#pragma once

#include "compiler/program.hpp"
#include "engine/sample.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace flows {

enum class EventType { note_on, note_off };
struct Event {
    std::uint64_t sample = 0;
    EventType type = EventType::note_on;
    std::uint64_t note_id = 0;
    double pitch = 69.0;
    double velocity = 1.0;
};

struct OutputGainGroup { std::uint64_t mask=0, value=0; float gain=1; };

struct VoiceIdentity {
    std::uint32_t slot = 0;
    std::uint64_t generation = 0;
    std::uint64_t note_id = 0;
};

class CaptureSink {
public:
    virtual ~CaptureSink() = default;
    // Called after the producer, before scratch reuse. Implementations must be bounded.
    virtual void capture(ValueId value, VoiceIdentity voice, std::uint64_t start_sample,
                         std::span<const float> samples) noexcept = 0;
    // Stereo-aware sinks override this. Existing sinks receive bounded mono chunks.
    virtual void capture_stereo(ValueId value, VoiceIdentity voice, std::uint64_t start_sample,
                                std::span<const float> left, std::span<const float> right) noexcept;
    virtual void event(const Event& event, VoiceIdentity voice,
                       std::string_view action) noexcept = 0;
};

struct EngineConfig {
    double sample_rate = 48000.0;
    std::uint32_t max_block_size = 256;
    std::uint32_t max_voices = 32;
    // Playback boundary correction plus an onset window of one third this time;
    // zero preserves exact offline DSP output.
    // The application enables 3 ms. Captured signal nodes are always unmodified.
    double boundary_seconds = 0.0;
};

struct Transport {
    double bpm = 120.0;
    double ppq = 0.0;
    bool playing = true;
    bool positioned = false;
};

struct EngineStats {
    std::uint64_t processed_samples = 0;
    std::uint64_t accepted_notes = 0;
    std::uint64_t rejected_notes = 0;
    std::uint64_t completed_notes = 0;
    std::uint32_t active_voices = 0;
    std::uint32_t voice_instance_count = 0;
    std::uint64_t process_errors = 0;
    std::uint64_t parameter_updates = 0;
    std::uint64_t parameter_epoch = 0;
};

enum class ProcessError {
    none, block_capacity, event_capacity, event_order, event_range,
    invalid_note, duplicate_note, unknown_note, held_capacity,
    invalid_parameter, parameter_capacity, nonfinite_output
};

struct ProcessResult {
    ProcessError error = ProcessError::none;
    constexpr explicit operator bool() const noexcept { return error == ProcessError::none; }
};

struct ParameterUpdate {
    std::uint64_t sample = 0;
    ValueId parameter = 0;
    double value = 0.0;
    // null: authored smoothing; 0: immediate; N: target at sample + N exactly.
    std::optional<std::uint32_t> ramp_samples = std::nullopt;
    bool ramp_logarithmic = false;
    std::optional<double> ramp_start = std::nullopt;
};

class Engine {
public:
    Engine(const Program& program, EngineConfig config, SampleBank samples = {});
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Event times are absolute, sorted, and inside this block's sample interval.
    // All memory is prepared by the constructor. Invalid blocks panic, emit silence,
    // advance the clock and return an error; they never throw on the audio thread.
    ProcessResult process(std::span<const Event> events, std::span<float> output,
                          CaptureSink* captures = nullptr,
                          std::span<const ParameterUpdate> parameters = {}) noexcept;
    ProcessResult process_stereo(std::span<const Event> events, std::span<float> left,
                                 std::span<float> right, CaptureSink* captures = nullptr,
                                 std::span<const ParameterUpdate> parameters = {}) noexcept;
    // Supplied PPQ is the next block's first sample. Without it the clock continues.
    void set_transport(Transport transport) noexcept;
    // Host output faders. Probes remain upstream of these smoothed gains.
    void set_output_gains(std::span<const OutputGainGroup> groups, float fallback = 1) noexcept;
    // Hard reset for host stop / lost events. Keeps the sample clock and parameters.
    void panic() noexcept;
    // Audio-thread subscription update; uses prepared storage and preserves DSP state.
    // Until called, offline capture includes all authored named signals.
    void set_capture_values(std::span<const ValueId> values) noexcept;
    // Replay an accepted held ledger into a fresh/reset engine without advancing time.
    // Up to 1024 note_on identities; admission callbacks expose any new policy rejection.
    ProcessResult restore_held_notes(std::span<const Event> notes,
                                     CaptureSink* captures = nullptr,
                                     std::span<const ParameterUpdate> initial_parameters = {}) noexcept;
    const EngineStats& stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace flows
