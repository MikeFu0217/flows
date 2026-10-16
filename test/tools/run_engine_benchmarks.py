#!/usr/bin/env python3
"""Run profiles.json against the standalone engine microbenchmark (not an audio host)."""
import argparse
import datetime
import hashlib
import itertools
import json
import math
from pathlib import Path
import platform
import subprocess


def run(command):
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout.strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--duration", type=float, help="override measured seconds; default uses profile")
    parser.add_argument("--warmup", type=float, help="override warmup seconds; default uses profile")
    parser.add_argument("--build-flags", default="unknown; supply actual binary build flags for comparisons")
    args = parser.parse_args()
    if args.output.exists() and any(args.output.iterdir()): parser.error("output must be empty")
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    profile_path = root / "test/performance/profiles.json"
    profiles = json.loads(profile_path.read_text())
    try:
        cpu = run(["sysctl", "-n", "machdep.cpu.brand_string"]) if platform.system() == "Darwin" else platform.processor()
    except (OSError, subprocess.CalledProcessError):
        cpu = "model unavailable in sandbox; architecture " + platform.machine()
    revision = run(["git", "-C", str(root), "rev-parse", "HEAD"])
    metadata = {
        "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "machine": platform.machine(), "os": platform.platform(), "cpu": cpu,
        "audio_driver": "not_applicable_offline", "host_version": "not_applicable_offline",
        "build_type": "standalone microbenchmark", "build_flags": args.build_flags,
        "compiler_toolchain": run(["clang++", "--version"]), "dependency_versions": "C++ standard library only",
        "backend": "native-block-v1", "optimization_passes": "topological block kernels; unchanged-input conversion caches; see build_flags",
        "engine_revision": revision, "compiler_revision": revision,
        "working_tree": "dirty; source hashes below" if run(["git", "-C", str(root), "status", "--porcelain"]) else "clean",
        "binary_sha256": digest(args.binary), "profiles_sha256": digest(profile_path),
        "source_hashes": {str(p.relative_to(root)): digest(p) for p in
                          [root / "src/compiler/program.cpp", root / "src/compiler/program.hpp",
                           root / "src/engine/engine.cpp", root / "src/engine/engine.hpp",
                           root / "test/cpp/engine_benchmark.cpp"]},
        "analysis_windows": [], "gui_workload": "not part of timed path; other desktop tasks may be active",
        "dependency_hash": "not_applicable_no_external_DSP_dependencies", "asset_hashes": [],
        "dsp_program_digest": "not_measured; deterministic source and compiler source hashes recorded",
        "parameter_epoch": 0, "control_epoch": 0, "observation_digest": "first eight unique named signal roots, explicitly subscribed; mono/stereo channels retained",
        "channel_count": "per result; source output channel count",
    }
    report = {"scope": "engine.process plus bounded synchronous capture only; excludes GUI, analyzer, device, plugin wrapper and hot-swap",
              "duration_units": "warmup/duration are rendered audio seconds, not elapsed wall time",
              "metadata": metadata, "results": [], "unmeasured_metrics": ["scratch_buffer_bytes", "voice_state_bytes", "retired_program_bytes_peak"],
              "status": "measured", "budget_scope": "microbenchmark only; passing does not certify device deadline performance"}
    for profile in profiles["benchmarks"]:
        duration = args.duration if args.duration is not None else profile["duration_seconds"]
        warmup = args.warmup if args.warmup is not None else profile["warmup_seconds"]
        source = (profile_path.parent / profile["preset"]).resolve()
        for rate, block, voices, captures in itertools.product(profile["sample_rates"], profile["block_sizes"], profile["voice_counts"], [False, True]):
            events_file = None
            fixture_hash = "generated_held_chord_pitch48_plus_index_velocity0.1"
            if profile["workload"]["kind"] == "repeating_case":
                fixture = (profile_path.parent / profile["workload"]["case"]).resolve()
                fixture_hash = digest(fixture)
                case = json.loads(fixture.read_text())
                period = profile["workload"]["period_seconds"]
                records = []
                for cycle in range(math.ceil((warmup + duration) / period)):
                    ids = {}
                    for event in case["events"]:
                        timestamp = cycle * period + event["time_seconds"]
                        if timestamp >= warmup + duration: continue
                        identity = ids.setdefault(event["note_id"], cycle * 1000 + len(ids) + 1)
                        sample = math.floor(timestamp * rate + 0.5)
                        records.append(f"{sample} {event['type']} {identity} {event['pitch']} {event.get('velocity', 0.0)}\n")
                events_file = args.output / f"events-{profile['id']}-{rate}.tsv"
                events_file.write_text("".join(records))
            command = [str(args.binary.resolve()), str(source), str(rate), str(block), str(voices), str(warmup), str(duration),
                       str(int(captures)), str(events_file.resolve()) if events_file else "-"]
            measured = json.loads(run(command))
            deadline_us = block / rate * 1e6
            passed = measured["render_time_p99_us"] <= deadline_us * profiles["budgets"]["render_time_p99_deadline_fraction"] and \
                measured["deadline_misses"] == measured["rt_allocations"] == measured["rt_deallocations"] == 0
            measured.update(id=profile["id"], sample_rate=rate, block_size=block, voice_count=voices,
                            probe_enabled=captures, warmup_seconds=warmup, duration_seconds=duration, seed=profile["seed"],
                            source_hash=digest(source), event_fixture_hash=fixture_hash,
                            event_input_hash=digest(events_file) if events_file else fixture_hash,
                            capture_config={"kind": "bounded synchronous sum", "signals": measured.get("capture_signals", measured["capture_channels"]), "channels": measured["capture_channels"],
                                            "voice_selector": "mixed", "ring_duration": "one callback; always consumed", "sample_rate": rate},
                            render_time_p99_deadline_fraction=measured["render_time_p99_us"] / deadline_us,
                            budget_status="passed" if passed else "failed")
            report["results"].append(measured)
            print(f"{profile['id']} {rate}/{block} voices={voices} probes={captures}: p99={measured['render_time_p99_us']:.2f}us {measured['budget_status']}", flush=True)
    report["all_microbenchmark_budgets_passed"] = all(r["budget_status"] == "passed" for r in report["results"])
    # The capture-on/off pairing must produce identical instrument output.
    report["capture_output_invariant"] = all(all(a.get(key) == b.get(key) for key in ("checksum", "checksum_left", "checksum_right", "output_hash")) for a, b in zip(report["results"][::2], report["results"][1::2]))
    (args.output / "benchmark.json").write_text(json.dumps(report, indent=2) + "\n")
    return int(not report["capture_output_invariant"])


if __name__ == "__main__":
    raise SystemExit(main())
