#!/usr/bin/env python3
"""Render a real Flows fixture and inspect its PCM16 output using only stdlib."""

import argparse
import array
import hashlib
import json
import math
from pathlib import Path
import platform
import subprocess
import sys
import wave

from validate_fixtures import load_json, validate_case


SUPPORTED_CHECKS = {
    "finite_audio", "peak_limit", "dominant_frequency",
    "silence_window", "manual_listen",
}


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sample_index(seconds, sample_rate):
    return math.floor(seconds * sample_rate + 0.5)


def read_audio(path):
    with wave.open(str(path), "rb") as handle:
        if (handle.getnchannels(), handle.getsampwidth(), handle.getcomptype()) != (1, 2, "NONE"):
            raise ValueError("renderer output must be uncompressed mono PCM16 WAV")
        sample_rate = handle.getframerate()
        frame_count = handle.getnframes()
        raw = handle.readframes(frame_count)
    if len(raw) != frame_count * 2:
        raise ValueError("WAV is truncated")
    values = array.array("h")
    values.frombytes(raw)
    if sys.byteorder != "little":
        values.byteswap()
    return sample_rate, [value / 32768.0 for value in values]


def positive_crossing_frequency(values, sample_rate):
    """Period estimate for the single-tone reference; not an FFT spectrum peak."""
    crossings = []
    for index in range(1, len(values)):
        left, right = values[index - 1], values[index]
        if left <= 0 < right:
            crossings.append(index - 1 - left / (right - left))
    if len(crossings) < 3:
        raise ValueError("frequency check needs at least three positive zero crossings")
    return sample_rate * (len(crossings) - 1) / (crossings[-1] - crossings[0])


def evaluate_check(check, values, sample_rate, engine):
    kind = check["kind"]
    result = {"kind": kind, "status": "failed"}
    if kind not in SUPPORTED_CHECKS:
        raise ValueError(f"unsupported check: {kind}")
    if kind == "manual_listen":
        return {"kind": kind, "status": "pending_review", "description": check["description"]}
    if kind == "finite_audio":
        passed = all(math.isfinite(value) for value in values)
        result.update(actual_nonfinite=sum(not math.isfinite(value) for value in values),
                      scope="exported PCM16 samples; does not inspect pre-quantization DSP values")
    elif kind == "peak_limit":
        peak = max((abs(value) for value in values), default=0.0)
        raw_peak = engine.get("peak_before_quantization")
        if type(raw_peak) not in (int, float) or not math.isfinite(raw_peak) or raw_peak < 0:
            raise ValueError("peak_limit needs a finite pre-quantization peak from the engine")
        passed = max(peak, raw_peak) <= check["max_abs"]
        result.update(actual_pcm_peak=peak, actual_peak_before_quantization=raw_peak,
                      max_abs=check["max_abs"])
    else:
        begin = sample_index(check["start_seconds"], sample_rate)
        end = sample_index(check["end_seconds"], sample_rate)
        if not 0 <= begin < end <= len(values):
            raise ValueError(f"{kind}: empty or out-of-bounds sample window")
        window = values[begin:end]
        result.update(start_sample=begin, end_sample=end)
        if kind == "silence_window":
            rms = math.sqrt(math.fsum(value * value for value in window) / len(window))
            passed = rms <= check["max_rms"]
            result.update(actual_rms=rms, max_rms=check["max_rms"])
        else:
            measured = positive_crossing_frequency(window, sample_rate)
            error = abs(measured - check["hz"])
            passed = error <= check["tolerance_hz"]
            result.update(actual_hz=measured, expected_hz=check["hz"],
                          error_hz=error, tolerance_hz=check["tolerance_hz"],
                          method="interpolated positive zero crossings; single-tone period estimate")
    result["status"] = "passed" if passed else "failed"
    return result


def run_case(case_path, binary, output):
    case_path, binary, output = case_path.resolve(), binary.resolve(), output.resolve()
    if output.exists() and any(output.iterdir()):
        raise ValueError("output directory must be empty to prevent stale artifacts being reported")
    output.mkdir(parents=True, exist_ok=True)
    report = {"schema_version": 1, "status": "failed", "checks": [],
              "manual_review_status": "not_run", "case": str(case_path)}
    exit_code = 1
    try:
        case = load_json(case_path)
        for check in case.get("checks", []):
            if check.get("kind") not in SUPPORTED_CHECKS:
                raise ValueError(f"unsupported check: {check.get('kind')}")
        # Case fixtures use the documented test/{cases,presets} layout.
        validate_case(case, case_path, case_path.parent.parent)
        source = (case_path.parent / case["preset"]).resolve()
        render = case["render"]
        if render["channels"] != 1:
            raise ValueError("the current renderer supports mono only")
        sample_rate = render["sample_rate"]
        note_ids = {}
        records = []
        for event in case["events"]:
            identity = note_ids.setdefault(event["note_id"], len(note_ids) + 1)
            sample = sample_index(event["time_seconds"], sample_rate)
            velocity = event.get("velocity", 0.0)
            records.append(f"{sample} {event['type']} {identity} {event['pitch']} {velocity:.17g}\n")
        events_path = output / "events-input.tsv"
        events_path.write_text("".join(records), encoding="utf-8")
        report.update(case_id=case["id"], behavior={"status": "not_run", "expectations": case["behavior"]})
        report["fingerprint"] = {
            "source_sha256": sha256(source), "binary_sha256": sha256(binary),
            "case_sha256": sha256(case_path), "events_sha256": sha256(events_path),
            "render": render, "observations": case["observations"],
            "note_id_map": note_ids, "platform": platform.platform(),
            "python": platform.python_version(),
            "event_rounding": "floor(time_seconds * sample_rate + 0.5)",
        }
        command = [str(binary), "render", str(source), "--events", str(events_path),
                   "--output", str(output), "--sample-rate", str(sample_rate),
                   "--block-size", str(render["block_size"]),
                   "--duration", str(render["duration_seconds"]),
                   "--observe", ",".join(case["observations"])]
        result = subprocess.run(command, text=True, capture_output=True, timeout=120, check=False)
        report["execution"] = {"command": command, "returncode": result.returncode,
                               "stdout": result.stdout, "stderr": result.stderr}
        if result.returncode != 0:
            raise ValueError(f"renderer failed with exit code {result.returncode}")
        required = ("output.wav", "program.ir.json", "captures.csv", "events.csv", "engine.json")
        for name in required:
            if not (output / name).is_file():
                raise ValueError(f"renderer did not produce {name}")
        ir = load_json(output / "program.ir.json")
        engine = load_json(output / "engine.json")
        if not isinstance(ir, dict) or not isinstance(engine, dict):
            raise ValueError("IR and engine report must be JSON objects")
        actual_rate, values = read_audio(output / "output.wav")
        expected_frames = sample_index(render["duration_seconds"], sample_rate)
        if actual_rate != sample_rate or len(values) != expected_frames:
            raise ValueError(f"WAV config mismatch: {actual_rate} Hz, {len(values)} frames; "
                             f"expected {sample_rate} Hz, {expected_frames} frames")
        report["engine"] = engine
        report["fingerprint"]["actual_engine_config"] = {
            key: engine[key] for key in ("backend", "sample_rate", "block_size", "channels", "duration_seconds")
            if key in engine
        }
        report["audio"] = {"frames": len(values), "sample_rate": actual_rate,
                           "channels": 1, "format": "PCM16", "quantization_step": 1 / 32768,
                           "amplitude_normalization": "signed PCM16 / 32768"}
        report["artifacts"] = {name: {"path": name, "sha256": sha256(output / name)} for name in required}
        for check in case["checks"]:
            try:
                report["checks"].append(evaluate_check(check, values, sample_rate, engine))
            except ValueError as error:
                report["checks"].append({"kind": check["kind"], "status": "failed", "error": str(error)})
        report["status"] = "failed" if any(check["status"] == "failed" for check in report["checks"]) else "passed"
        report["manual_review_status"] = "pending_review" if any(
            check["status"] == "pending_review" for check in report["checks"]) else "not_requested"
        report["status_scope"] = "automatic artifact checks only; behavior traces and manual listening are separate"
        exit_code = int(report["status"] != "passed")
    except (OSError, ValueError, KeyError, TypeError, wave.Error, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    (output / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")
    return exit_code, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", required=True, type=Path)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path, help="new or empty output directory")
    args = parser.parse_args()
    try:
        code, report = run_case(args.case, args.binary, args.output)
    except (OSError, ValueError) as error:
        print(f"Case runner: {error}", file=sys.stderr)
        return 1
    print(f"Automatic checks: {report['status']}; manual listening: {report['manual_review_status']}")
    print(args.output.resolve() / "report.json")
    if "error" in report:
        print(report["error"], file=sys.stderr)
    return code


if __name__ == "__main__":
    sys.exit(main())
