#!/usr/bin/env python3
"""Independent analytic tests against the compiled Flows CLI; stdlib only."""

import argparse
import csv
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import wave


BINARY = None
RATE = 48000
FRAME_COUNT = 9600
NOTE_ON = 137
NOTE_OFF = 7001
VELOCITY = 0.8
QUANTIZATION_TOLERANCE = 2 / 32768


def instrument(level="-6db", pitch="key.pitch"):
    return f"""import standard as std
from lib.osc import ClassicSine

Inst.sigstream(std.event.keys.poly()) as key:
    mOsc1 = ClassicSine.make()
    raw = mOsc1(pitch: {pitch}, unison: 1, level: {level})
    return raw * key.gate * key.velocity
"""


def decoded_wav(path):
    with wave.open(str(path), "rb") as handle:
        assert handle.getnchannels() == 1 and handle.getsampwidth() == 2
        assert handle.getframerate() == RATE
        count = handle.getnframes()
        data = handle.readframes(count)
    return struct.unpack(f"<{count}h", data)


class SinePipeline(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="flows-sine-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def command(self, *arguments):
        return subprocess.run([str(BINARY), *map(str, arguments)], text=True,
                              capture_output=True, timeout=60, check=False)

    def render(self, name, source=None, block_size=64):
        source_path = self.root / f"{name}.flows"
        source_path.write_text(source or instrument(), encoding="utf-8")
        event_path = self.root / f"{name}.tsv"
        event_path.write_text(f"{NOTE_ON} note_on 1 69 {VELOCITY}\n"
                              f"{NOTE_OFF} note_off 1 69 0\n", encoding="utf-8")
        output = self.root / name
        result = self.command("render", source_path, "--events", event_path, "--output", output,
                              "--sample-rate", RATE, "--block-size", block_size,
                              "--duration", FRAME_COUNT / RATE, "--observe", "key.pitch,key.gate,raw")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return decoded_wav(output / "output.wav"), output

    def assert_analytic(self, pcm, frequency=440.0, level_db=-6.0):
        self.assertEqual(len(pcm), FRAME_COUNT)
        amplitude = 10 ** (level_db / 20) * VELOCITY
        error = 0.0
        for index, sample in enumerate(pcm):
            expected = (amplitude * math.sin(2 * math.pi * frequency * (index - NOTE_ON) / RATE)
                        if NOTE_ON <= index < NOTE_OFF else 0.0)
            error = max(error, abs(sample / 32768 - expected))
        self.assertLessEqual(error, QUANTIZATION_TOLERANCE,
                             f"max analytic error {error} exceeds PCM16 tolerance")

    def test_compile_and_analytic_sine(self):
        source = self.root / "compile.flows"
        source.write_text(instrument(), encoding="utf-8")
        ir_path = self.root / "compiled.ir.json"
        compiled = self.command("compile", source, "--ir", ir_path)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        self.assertIsInstance(json.loads(ir_path.read_text()), dict)
        pcm, output = self.render("reference")
        self.assert_analytic(pcm)
        with (output / "captures.csv").open(newline="") as handle:
            reader = csv.DictReader(handle)
            self.assertEqual(reader.fieldnames, ["sample", "value_id", "voice_slot", "voice_generation", "note_id", "value"])
            first = next(reader, None)
            self.assertIsNotNone(first, "requested captures are empty")
            self.assertTrue(math.isfinite(float(first["value"])))
        with (output / "events.csv").open(newline="") as handle:
            events = list(csv.DictReader(handle))
        self.assertIn(NOTE_ON, [int(event["sample"]) for event in events])
        self.assertIn(NOTE_OFF, [int(event["sample"]) for event in events])
        self.assertIsInstance(json.loads((output / "engine.json").read_text()), dict)

    def test_source_level_and_pitch_edits_change_audio(self):
        baseline, _ = self.render("baseline")
        quieter, _ = self.render("quieter", instrument(level="-12db"))
        higher, _ = self.render("higher", instrument(pitch="key.pitch + 12"))
        self.assertNotEqual(baseline, quieter)
        self.assertNotEqual(baseline, higher)
        self.assert_analytic(quieter, level_db=-12)
        self.assert_analytic(higher, frequency=880)

    def test_invalid_unit_rejected_without_audio(self):
        source = self.root / "invalid.flows"
        source.write_text(instrument(level="-6hz"), encoding="utf-8")
        ir_path = self.root / "invalid.ir.json"
        compiled = self.command("compile", source, "--ir", ir_path)
        self.assertNotEqual(compiled.returncode, 0)
        self.assertFalse(ir_path.exists())
        self.assertTrue((compiled.stdout + compiled.stderr).strip(), "missing compiler diagnostics")
        events = self.root / "invalid.tsv"
        events.write_text("0 note_on 1 69 0.8\n2400 note_off 1 69 0\n", encoding="utf-8")
        output = self.root / "invalid-output"
        rendered = self.command("render", source, "--events", events, "--output", output,
                                "--sample-rate", RATE, "--block-size", 64, "--duration", 0.1,
                                "--observe", "raw")
        self.assertNotEqual(rendered.returncode, 0)
        self.assertFalse((output / "output.wav").exists())
        self.assertTrue((rendered.stdout + rendered.stderr).strip(), "missing render diagnostics")

    def test_block_partition_invariance(self):
        reference, _ = self.render("block1", block_size=1)
        self.assert_analytic(reference)
        for size in (64, 127):
            with self.subTest(block_size=size):
                actual, _ = self.render(f"block{size}", block_size=size)
                self.assertEqual(reference, actual, "block boundaries changed the PCM16 signal")

    def test_real_case_runner_reports_unreviewed_listening(self):
        tools = Path(__file__).resolve().parent
        output = self.root / "fixture"
        result = subprocess.run([sys.executable, str(tools / "run_case.py"),
                                 "--case", str(tools.parent / "cases/sine-a4-reference.json"),
                                 "--binary", str(BINARY), "--output", str(output)],
                                text=True, capture_output=True, timeout=120, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads((output / "report.json").read_text())
        self.assertEqual(report["status"], "passed")
        self.assertEqual(report["manual_review_status"], "pending_review")
        self.assertEqual(report["behavior"]["status"], "not_run")
        self.assertEqual(len(report["fingerprint"]["binary_sha256"]), 64)
        checks = {check["kind"]: check for check in report["checks"]}
        self.assertEqual(checks["dominant_frequency"]["status"], "passed")
        self.assertEqual(checks["manual_listen"]["status"], "pending_review")

    def test_case_runner_rejects_unsupported_check(self):
        tools = Path(__file__).resolve().parent
        case = json.loads((tools.parent / "cases/sine-a4-reference.json").read_text())
        case["checks"] = [{"kind": "unimplemented_check"}]
        case_path = self.root / "unsupported.json"
        case_path.write_text(json.dumps(case), encoding="utf-8")
        output = self.root / "unsupported-check"
        result = subprocess.run([sys.executable, str(tools / "run_case.py"),
                                 "--case", str(case_path), "--binary", str(BINARY),
                                 "--output", str(output)], text=True, capture_output=True,
                                timeout=60, check=False)
        self.assertNotEqual(result.returncode, 0)
        report = json.loads((output / "report.json").read_text())
        self.assertEqual(report["status"], "failed")
        self.assertIn("unsupported check", report["error"])
        self.assertFalse((output / "output.wav").exists())


def main():
    global BINARY
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    args, remaining = parser.parse_known_args()
    BINARY = args.binary.resolve()
    if not BINARY.is_file():
        parser.error(f"binary does not exist: {BINARY}")
    unittest.main(argv=[sys.argv[0], *remaining])


if __name__ == "__main__":
    main()
