#!/usr/bin/env python3
"""Validate fixture metadata only; no Flows compiler or audio engine is run."""

import argparse
import json
import math
from pathlib import Path
import sys


class InvalidFixture(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise InvalidFixture(message)


def object_value(value, label):
    require(isinstance(value, dict), f"{label} must be an object")
    return value


def fields(value, names, label):
    object_value(value, label)
    missing = set(names) - value.keys()
    require(not missing, f"{label} missing fields: {', '.join(sorted(missing))}")


def string(value, label):
    require(isinstance(value, str) and bool(value.strip()),
            f"{label} must be a nonempty string")
    return value


def number(value, label, minimum=None, maximum=None, integer=False):
    valid_type = type(value) is int if integer else type(value) in (int, float)
    require(valid_type, f"{label} must be {'an integer' if integer else 'a number'}")
    try:
        finite = math.isfinite(value)
    except OverflowError:
        finite = False
    require(finite, f"{label} must be finite")
    if minimum is not None:
        require(value >= minimum, f"{label} must be >= {minimum}")
    if maximum is not None:
        require(value <= maximum, f"{label} must be <= {maximum}")
    return value


def array(value, label, nonempty=False):
    require(isinstance(value, list), f"{label} must be an array")
    require(not nonempty or bool(value), f"{label} must not be empty")
    return value


def string_array(value, label, nonempty=False):
    array(value, label, nonempty)
    for index, item in enumerate(value):
        string(item, f"{label}[{index}]")
    require(len(value) == len(set(value)), f"{label} must not contain duplicates")


def file_reference(value, source, root, suffix, label):
    reference = Path(string(value, label))
    require(not reference.is_absolute(), f"{label} must be relative to its JSON file")
    target = (source.parent / reference).resolve()
    require(target.is_relative_to(root), f"{label} must stay inside the test directory")
    require(target.suffix == suffix and target.is_file(),
            f"{label} does not reference an existing {suffix} file: {value}")
    return target


def preset_reference(value, source, root):
    return file_reference(value, source, root, ".flows", "preset")


def validate_case(case, source, root):
    fields(case, ("schema_version", "id", "preset", "render", "events",
                  "observations", "checks"), "case")
    require(type(case["schema_version"]) is int and case["schema_version"] == 1,
            "case.schema_version must be integer 1")
    case_id = string(case["id"], "case.id")
    preset_reference(case["preset"], source, root)

    render = case["render"]
    fields(render, ("sample_rate", "block_size", "channels", "duration_seconds", "seed"),
           "render")
    rate = number(render["sample_rate"], "render.sample_rate", minimum=1, integer=True)
    number(render["block_size"], "render.block_size", minimum=1, integer=True)
    number(render["channels"], "render.channels", minimum=1, integer=True)
    duration = number(render["duration_seconds"], "render.duration_seconds", minimum=0)
    require(duration > 0, "render.duration_seconds must be positive")
    number(render["seed"], "render.seed", minimum=0, integer=True)

    active = {}
    used_ids = set()
    last_time = -1
    for index, event in enumerate(array(case["events"], "events", nonempty=True)):
        label = f"events[{index}]"
        fields(event, ("time_seconds", "type", "note_id", "pitch"), label)
        event_time = number(event["time_seconds"], f"{label}.time_seconds", minimum=0)
        require(event_time < duration, f"{label} must occur before render ends")
        require(event_time >= last_time, "events must be sorted by time_seconds")
        last_time = event_time
        event_type = string(event["type"], f"{label}.type")
        require(event_type in ("note_on", "note_off"), f"{label}.type is unsupported")
        note_id = string(event["note_id"], f"{label}.note_id")
        pitch = number(event["pitch"], f"{label}.pitch", minimum=0, maximum=127,
                       integer=True)
        if event_type == "note_on":
            fields(event, ("velocity",), label)
            number(event["velocity"], f"{label}.velocity", minimum=0, maximum=1)
            require(note_id not in used_ids, f"{label} reuses note_id {note_id!r}")
            used_ids.add(note_id)
            active[note_id] = pitch
        else:
            require("velocity" not in event, f"{label}: velocity is only valid for note_on")
            require(note_id in active, f"{label} has no active note_on for {note_id!r}")
            require(active[note_id] == pitch, f"{label}.pitch differs from its note_on")
            del active[note_id]
    require(not active, f"unpaired note_on ids: {', '.join(sorted(active))}")

    string_array(case["observations"], "observations")
    if "behavior" in case:
        string_array(case["behavior"], "behavior")
    for index, check in enumerate(array(case["checks"], "checks", nonempty=True)):
        label = f"checks[{index}]"
        fields(check, ("kind",), label)
        kind = string(check["kind"], f"{label}.kind")
        require(kind in ("finite_audio", "peak_limit", "silence_window",
                         "dominant_frequency", "manual_listen"),
                f"{label}.kind is unsupported: {kind}")
        if kind == "peak_limit":
            fields(check, ("max_abs",), label)
            number(check["max_abs"], f"{label}.max_abs", minimum=0)
        elif kind in ("silence_window", "dominant_frequency"):
            fields(check, ("start_seconds", "end_seconds"), label)
            start = number(check["start_seconds"], f"{label}.start_seconds", minimum=0)
            end = number(check["end_seconds"], f"{label}.end_seconds", maximum=duration)
            require(start < end, f"{label} requires start_seconds < end_seconds")
            if kind == "silence_window":
                fields(check, ("max_rms",), label)
                number(check["max_rms"], f"{label}.max_rms", minimum=0)
            else:
                fields(check, ("hz", "tolerance_hz"), label)
                hz = number(check["hz"], f"{label}.hz", minimum=0, maximum=rate / 2)
                tolerance = number(check["tolerance_hz"], f"{label}.tolerance_hz", minimum=0)
                require(hz > 0 and tolerance > 0, f"{label}: hz and tolerance_hz must be positive")
        elif kind == "manual_listen":
            fields(check, ("description",), label)
            string(check["description"], f"{label}.description")
    return case_id


def validate_profiles(profiles, source, root):
    fields(profiles, ("schema_version", "benchmarks", "metrics", "budgets",
                      "required_result_metadata"), "performance profiles")
    require(type(profiles["schema_version"]) is int and profiles["schema_version"] == 1,
            "profiles.schema_version must be integer 1")
    string_array(profiles["metrics"], "metrics", nonempty=True)
    string_array(profiles["required_result_metadata"], "required_result_metadata", nonempty=True)
    budgets = profiles["budgets"]
    fields(budgets, ("render_time_p99_deadline_fraction", "render_time_max_deadline_fraction",
                     "rt_allocations", "deadline_misses"), "budgets")
    p99 = number(budgets["render_time_p99_deadline_fraction"],
                 "budgets.render_time_p99_deadline_fraction", minimum=0, maximum=1)
    maximum = number(budgets["render_time_max_deadline_fraction"],
                     "budgets.render_time_max_deadline_fraction", minimum=0, maximum=1)
    require(0 < p99 <= maximum, "budgets requires 0 < p99 fraction <= max fraction")
    for field in ("rt_allocations", "deadline_misses"):
        number(budgets[field], f"budgets.{field}", minimum=0, integer=True)

    ids = set()
    benchmarks = array(profiles["benchmarks"], "benchmarks", nonempty=True)
    for index, benchmark in enumerate(benchmarks):
        label = f"benchmarks[{index}]"
        fields(benchmark, ("id", "preset", "sample_rates", "block_sizes", "voice_counts",
                           "duration_seconds", "warmup_seconds", "seed", "workload"), label)
        benchmark_id = string(benchmark["id"], f"{label}.id")
        require(benchmark_id not in ids, f"duplicate benchmark id: {benchmark_id}")
        ids.add(benchmark_id)
        preset = preset_reference(benchmark["preset"], source, root)
        for field in ("sample_rates", "block_sizes", "voice_counts"):
            values = array(benchmark[field], f"{label}.{field}", nonempty=True)
            for item in values:
                number(item, f"{label}.{field} item", minimum=1, integer=True)
            require(len(values) == len(set(values)), f"{label}.{field} contains duplicates")
        duration = number(benchmark["duration_seconds"], f"{label}.duration_seconds", minimum=0)
        require(duration > 0, f"{label}.duration_seconds must be positive")
        number(benchmark["warmup_seconds"], f"{label}.warmup_seconds", minimum=0)
        number(benchmark["seed"], f"{label}.seed", minimum=0, integer=True)
        workload = benchmark["workload"]
        fields(workload, ("kind",), f"{label}.workload")
        kind = string(workload["kind"], f"{label}.workload.kind")
        require(kind in ("held_chord", "repeating_case"),
                f"{label}.workload.kind is unsupported: {kind}")
        if kind == "held_chord":
            fields(workload, ("pitch_start", "pitch_step", "velocity"), f"{label}.workload")
            start = number(workload["pitch_start"], f"{label}.workload.pitch_start",
                           minimum=0, maximum=127, integer=True)
            step = number(workload["pitch_step"], f"{label}.workload.pitch_step",
                          minimum=1, integer=True)
            number(workload["velocity"], f"{label}.workload.velocity", minimum=0, maximum=1)
            highest_pitch = start + step * (max(benchmark["voice_counts"]) - 1)
            require(highest_pitch <= 127,
                    f"{label}.workload exceeds MIDI pitch 127 at the largest voice_count")
        else:
            fields(workload, ("case", "period_seconds"), f"{label}.workload")
            case_path = file_reference(workload["case"], source, root, ".json",
                                       f"{label}.workload.case")
            case = load_json(case_path)
            validate_case(case, case_path, root)
            require(preset_reference(case["preset"], case_path, root) == preset,
                    f"{label}.workload.case references a different preset")
            period = number(workload["period_seconds"], f"{label}.workload.period_seconds",
                            minimum=0)
            require(period > max(event["time_seconds"] for event in case["events"]),
                    f"{label}.workload.period_seconds must exceed every case event time")
    return len(benchmarks)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON field: {key}")
        result[key] = value
    return result


def reject_constant(value):
    raise InvalidFixture(f"non-finite JSON number: {value}")


def load_json(path):
    with path.open(encoding="utf-8") as handle:
        return json.load(handle, object_pairs_hook=unique_object, parse_constant=reject_constant)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1],
                        help="test directory (defaults to this script's parent test directory)")
    root = parser.parse_args().root.resolve()
    paths = sorted((root / "cases").rglob("*.json"))
    errors = []
    if not paths:
        errors.append("cases: no JSON fixtures found")
    ids = set()
    for path in paths:
        try:
            case_id = validate_case(load_json(path), path, root)
            require(case_id not in ids, f"duplicate case id: {case_id}")
            ids.add(case_id)
        except (OSError, ValueError) as error:
            errors.append(f"{path.relative_to(root)}: {error}")
    profile_path = root / "performance" / "profiles.json"
    benchmark_count = 0
    try:
        benchmark_count = validate_profiles(load_json(profile_path), profile_path, root)
    except (OSError, ValueError) as error:
        errors.append(f"performance/profiles.json: {error}")
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        print("Fixture validation failed; compiler/audio/performance not run.", file=sys.stderr)
        return 1
    print(f"Validated {len(paths)} case fixtures and {benchmark_count} performance profiles.")
    print("Fixture validation only; compiler/audio/performance not run.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
