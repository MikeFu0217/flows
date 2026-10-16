#!/usr/bin/env python3
"""Render all nine fixtures, verify captured behavior, and compare block partitions.

This supplements run_case.py's audio checks. It does not mark manual listening
accepted, measure device latency, or infer performance from offline render speed.
"""
import argparse
import array
import csv
import json
import math
from pathlib import Path
import subprocess
import sys

from run_case import load_json, run_case, sample_index, sha256


def behavior(case, directory):
    ir = load_json(directory / "program.ir.json")
    engine = load_json(directory / "engine.json")
    names = {entry["value"]: entry["name"] for entry in ir["named_values"]}
    rate = case["render"]["sample_rate"]
    size = sample_index(case["render"]["duration_seconds"], rate)
    streams = {}
    nonfinite = 0
    with (directory / "captures.csv").open() as handle:
        for row in csv.DictReader(handle):
            key = (names[int(row["value_id"])], int(row["voice_slot"]), int(row["voice_generation"]))
            if key not in streams:
                streams[key] = array.array("f", [math.nan]) * size
            numeric = float(row["value"])
            nonfinite += not math.isfinite(numeric)
            streams[key][int(row["sample"])] = numeric
    with (directory / "events.csv").open() as handle:
        events = [{k: v if k == "action" else int(v) for k, v in row.items()} for row in csv.DictReader(handle)]
    checks = []

    def record(label, passed, **evidence):
        checks.append({"check": label, "status": "passed" if passed else "failed", **evidence})

    def point(name, seconds, expected, slot=0, sample=None, tolerance=2e-5):
        index = sample_index(seconds, rate) if sample is None else sample
        actual = streams.get((name, slot, 1), [])[index]
        record(f"{name}[{index}] voice {slot}", math.isfinite(actual) and abs(actual - expected) <= tolerance,
               actual=actual if math.isfinite(actual) else None, expected=expected, tolerance=tolerance)

    accepted = [event for event in events if event["action"] == "accepted"]
    released = [event for event in events if event["action"] == "released"]
    completed = [event for event in events if event["action"] == "completed"]
    expected_on = [sample_index(event["time_seconds"], rate) for event in case["events"] if event["type"] == "note_on"]
    expected_off = [sample_index(event["time_seconds"], rate) for event in case["events"] if event["type"] == "note_off"]
    record("exact admission/release event offsets", [e["sample"] for e in accepted] == expected_on and
           [e["sample"] for e in released] == expected_off, accepted=expected_on, released=expected_off)
    record("all notes accepted and voices complete", engine["accepted_notes"] == len(expected_on) and
           engine["rejected_notes"] == 0 and engine["active_voices"] == 0, engine=engine)
    record("all requested observations captured", set(case["observations"]) <= {key[0] for key in streams})
    record("captured signal values are finite", nonfinite == 0, actual_nonfinite=nonfinite)
    mono = ir["voice_policy"]["mode"] == "mono"
    identities = {(event["voice_slot"], event["voice_generation"]) for event in accepted}
    record("mono state identity persists" if mono else "poly note identities are independent",
           len(identities) == (1 if mono else len(expected_on)), identities=sorted(identities))
    if "key.trigger" in case["observations"]:
        expected_trigger = expected_on if ir["voice_policy"]["retrigger"] == "each_press" or not mono else expected_on[:1]
        actual_trigger = sorted(i for (name, _, _), data in streams.items() if name == "key.trigger"
                                for i, x in enumerate(data) if math.isfinite(x) and x != 0)
        record("single-sample trigger impulses", actual_trigger == expected_trigger,
               actual=actual_trigger, expected=expected_trigger)

    key = case["id"]
    if key == "mono-glide-retarget":
        for time, pitch in [(0.5, 69), (0.525, 69.75), (0.55, 70.5), (0.60, 72.25),
                            (0.65, 74), (0.75, 74), (0.80, 73), (0.85, 72), (1.0, 70.5), (1.05, 69)]:
            point("key.pitch", time, pitch)
        point("key.gate", 1.2, 0); point("amp", 1.35, 0.3); point("amp", 1.5, 0)
        completion_seconds = [1.5]
    elif key == "mono-release-during-glide":
        for time, pitch in [(0.5, 69), (0.52, 69.6), (0.55, 70.5), (0.575, 71.25), (0.6, 72), (0.8, 72)]:
            point("key.pitch", time, pitch)
        point("key.gate", 0.55, 0); point("amp", 0.7, 0.3); point("amp", 0.85, 0)
        completion_seconds = [0.85]
    elif key == "mono-priority-retrigger":
        for time, pitch, velocity in [(0.5, 72, 0.8), (1.0, 69, 0.4), (1.5, 69, 0.4)]:
            point("key.pitch", time, pitch); point("key.velocity", time, velocity)
        for time, amplitude in [(0.5, 0.6), (0.505, 0.8), (0.51, 1), (1.65, 0.3), (1.8, 0)]:
            point("amp", time, amplitude)
        point("key.gate", 1.0, 1); point("key.gate", 1.5, 0)
        completion_seconds = [1.8]
    elif key == "mono-repress-during-release":
        for time, amplitude in [(0.2, 0.6), (0.35, 0.3), (0.355, 0.65), (0.36, 1), (0.8, 0.3), (0.95, 0)]:
            point("amp", time, amplitude)
        point("key.pitch", 0.35, 72); point("key.gate", 0.35, 1)
        completion_seconds = [0.95]
    elif key == "poly-overlap-release":
        point("key.gate", 1.0, 1, slot=0); point("key.gate", 1.0, 0, slot=1)
        point("amp", 1.45, 0.3, slot=1); point("amp", 1.95, 0.3, slot=0)
        point("raw", 0.5, 0, slot=1)  # A new poly oscillator starts at phase zero.
        completion_seconds = [1.9, 2.4]
    elif key == "poly-same-pitch-identities":
        point("key.pitch", 0.4, 69, slot=0); point("key.pitch", 0.4, 69, slot=1)
        point("key.gate", 0.5, 0, slot=0); point("key.gate", 0.5, 1, slot=1)
        point("key.gate", 0.75, 0, slot=1)
        completion_seconds = [1.4, 1.65]
    elif key == "rolling-bass-kbbb-138":
        for onset, release in zip(expected_on, expected_off):
            for name in ("filter_env", "mid_amp", "sub_amp"):
                point(name, 0, 0, sample=onset)
            point("key.pitch", 0, 30, sample=onset)
            point("cutoff", 0, 200, sample=onset)
            for name, length in (("mid_amp", 0.020), ("sub_amp", 0.015)):
                end = release + sample_index(length, rate)
                point(name, 0, 0, sample=end)
                preceding = streams[(name, 0, 1)][end - 1]
                record(f"{name} retains its full release at {end - 1}", preceding > 0, actual=preceding)
        completion_seconds = None
        expected_complete = [release + sample_index(0.020, rate) for release in expected_off]
    else:
        point("key.pitch", 0.3, 69 if key == "sine-a4-reference" else 57)
        point("key.gate", 0.3, 1)
        completion_seconds = []  # Direct gate completion is already reported by release.
    if completion_seconds is not None:
        expected_complete = [sample_index(time, rate) for time in completion_seconds]
    record("exact completion offsets", [e["sample"] for e in completed] == expected_complete,
           actual=[e["sample"] for e in completed], expected=expected_complete)
    return {"status": "passed" if all(c["status"] == "passed" for c in checks) else "failed", "checks": checks,
            "scope": "listed captured control/envelope/identity/lifecycle assertions; timbre remains manual review"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new or empty output directory")
    parser.add_argument("--partition-blocks", default="1,127")
    args = parser.parse_args()
    if args.output.exists() and any(args.output.iterdir()):
        parser.error("output directory must be empty")
    args.output.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    summary = {"status": "passed", "binary_sha256": sha256(binary), "manual_listening": "pending_review", "cases": []}
    for case_path in sorted((Path(__file__).resolve().parent.parent / "cases").glob("*.json")):
        case = load_json(case_path)
        directory = args.output / case["id"]
        _, report = run_case(case_path, binary, directory)
        entry = {"id": case["id"], "audio_checks": report["status"], "partitions": []}
        try:
            entry["behavior"] = behavior(case, directory)
            source = (case_path.parent / case["preset"]).resolve()
            for block in [int(x) for x in args.partition_blocks.split(",") if x]:
                destination = directory / f"block-{block}"
                command = [str(binary), "render", str(source), "--events", str((directory / "events-input.tsv").resolve()),
                           "--output", str(destination.resolve()), "--sample-rate", str(case["render"]["sample_rate"]),
                           "--block-size", str(block), "--duration", str(case["render"]["duration_seconds"]), "--observe", ""]
                result = subprocess.run(command, capture_output=True, text=True, timeout=120)
                same = result.returncode == 0 and sha256(destination / "output.wav") == sha256(directory / "output.wav")
                entry["partitions"].append({"block_size": block, "status": "passed" if same else "failed",
                                            "criterion": "byte-identical PCM16 WAV", "stderr": result.stderr})
            entry["status"] = "passed" if report["status"] == entry["behavior"]["status"] == "passed" and all(
                p["status"] == "passed" for p in entry["partitions"]) else "failed"
        except (OSError, ValueError, IndexError, KeyError, subprocess.TimeoutExpired) as error:
            entry.update(status="failed", error=str(error))
        (directory / "behavior-report.json").write_text(json.dumps(entry, indent=2, allow_nan=False) + "\n")
        summary["cases"].append(entry)
        if entry["status"] != "passed": summary["status"] = "failed"
        print(f"{case['id']}: {entry['status']}", flush=True)
    (args.output / "suite.json").write_text(json.dumps(summary, indent=2, allow_nan=False) + "\n")
    return int(summary["status"] != "passed")


if __name__ == "__main__":
    sys.exit(main())
