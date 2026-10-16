#!/usr/bin/env python3
"""Exercise the native processor against a local deterministic Chat Completions peer."""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Peer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.checked: set[str] = set()
        self.errors: list[str] = []
        self.lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, value, status=200):
        encoded = json.dumps(value).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def completion(self, *, text=None, calls=None):
        message = {"role": "assistant", "content": text}
        if calls:
            message["tool_calls"] = [
                {"id": call_id, "type": "function", "function": {"name": name, "arguments": json.dumps(arguments)}}
                for call_id, name, arguments in calls
            ]
        self.reply({"choices": [{"index": 0, "message": message, "finish_reason": "tool_calls" if calls else "stop"}]})

    def stereo_render(self, outputs):
        """Check offline tools without changing the live host's instrument."""
        prefix = "import standard as std\nInst.sigstream(std.event.keys.poly()) as key:\n"
        anti_phase = prefix + "    image = stereo(0.25, -0.25) * key.gate\n    return image\n"
        mono = prefix + "    return key.gate * 0.25\n"
        clock = ("import standard as std\nfrom lib.mod import ClassicLFO\n"
                 "Inst.sigstream(std.event.keys.poly()) as key:\n"
                 "    lfo = ClassicLFO.make()\n"
                 "    motion = lfo(shape: sine, sync: 1/4, mode: transport, bipolar: true)\n"
                 "    return motion * key.gate\n")
        sample = ("import standard as std\nfrom lib.sample import ClassicSampler\n"
                  "Inst.sigstream(std.event.keys.poly()) as key:\n"
                  "    player = ClassicSampler.make()\n"
                  "    raw = player(file: \"Samples/flows-stereo.wav\", trigger: key.trigger, pitch: key.pitch, root: C3)\n"
                  "    return raw\n")
        held_note = [{"time_seconds": 0, "type": "note_on", "note_id": 1, "pitch": 69, "velocity": 1}]
        clock_args = {"source": clock, "duration_seconds": 0.125, "events": held_note}
        if not outputs:
            self.completion(calls=[
                ("anti", "render_case", {"source": anti_phase, "duration_seconds": 0.1, "events": held_note, "signals": ["image"]}),
                ("compare", "compare_observations", {"before_source": anti_phase, "after_source": mono, "duration_seconds": 0.1, "events": held_note}),
                ("slow", "render_case", dict(clock_args, bpm=60)),
                ("fast", "render_case", dict(clock_args, bpm=120)),
            ])
            return
        if "sample" not in outputs:
            self.completion(calls=[
                ("sample", "render_case", {"source": sample, "duration_seconds": 0.2, "events": held_note, "signals": ["raw"]}),
                ("sample_same", "compare_observations", {"before_source": sample, "after_source": sample, "duration_seconds": 0.1, "events": held_note}),
                ("missing_sample", "compile_snapshot", {"source": sample.replace("flows-stereo.wav", "missing-test.wav")}),
                ("catalogue", "read_source", {}),
            ])
            return
        sampled = outputs["sample"]
        assert sampled["status"] == "rendered" and sampled["channels"] == 2, f"AI sample render failed: {sampled}"
        assert sampled["metrics"]["finite"] and sampled["metrics"]["peak"] > .05
        assert sampled["observations"][0]["metrics"]["peak"] > .05, "AI sample probe was silent"
        assert outputs["sample_same"]["status"] == "compared" and outputs["sample_same"]["difference_peak"] == 0
        assert outputs["missing_sample"]["status"] == "failed" and "missing-test.wav" in outputs["missing_sample"]["error"]
        assert any(item["path"] == "Samples/flows-stereo.wav" for item in outputs["catalogue"]["samples"])
        anti = outputs["anti"]
        assert anti["status"] == "rendered", f"Stereo render failed: {anti}"
        assert anti["frames"] == 4800 and anti["channels"] == 2 and anti["acceptedNotes"] == 1
        assert anti["metricsScope"] == "both output channels" and anti["metrics"]["finite"]
        for field, expected in {"peak": 0.25, "rms": 0.25, "mean_power": 0.0625, "mean": 0}.items():
            assert abs(anti["metrics"][field] - expected) < 1e-7, f"Stereo {field} lost anti-phase channel energy"
        observed = anti["observations"][0]
        assert observed["signal"] == "image" and observed["projection"] == "mid" and observed["metrics"]["peak"] == 0
        compared = outputs["compare"]
        assert compared["status"] == "compared", f"Stereo comparison failed: {compared}"
        assert compared["frames"] == 4800 and compared["channels"] == 2, "Channel sample count was reported as timeline frames"
        assert abs(compared["difference_rms"] - math.sqrt(0.125)) < 1e-7, "Comparison must broadcast mono to both channels"
        assert abs(compared["difference_peak"] - 0.5) < 1e-7 and abs(compared["mean_power_difference"]) < 1e-7
        for name, bpm in [("slow", 60), ("fast", 120), ("default", 138)]:
            if name not in outputs:
                continue
            measured = outputs[name]
            assert measured["status"] == "rendered", f"Transport render failed: {measured}"
            assert measured["bpm"] == bpm and measured["frames"] == 6000 and measured["channels"] == 1
            samples = [math.sin(2 * math.pi * i * bpm / (60 * 48000)) for i in range(6000)]
            expected_rms = math.sqrt(sum(sample * sample for sample in samples) / len(samples))
            expected_mean = sum(samples) / len(samples)
            assert abs(measured["metrics"]["rms"] - expected_rms) < 1e-5, "Transport LFO ignored requested offline BPM or lost block phase"
            assert abs(measured["metrics"]["mean"] - expected_mean) < 1e-5, "Transport LFO started at the wrong PPQ"
        if "invalid_bpm" not in outputs:
            self.completion(calls=[
                ("default", "render_case", clock_args),
                ("invalid_bpm", "render_case", dict(clock_args, bpm=0)),
            ])
            return
        assert outputs["invalid_bpm"]["status"] == "failed" and "bpm" in outputs["invalid_bpm"]["error"], "Invalid tempo silently entered the offline engine"
        with self.server.lock:
            self.server.checked.add("stereo_render_tempo")
        self.completion(text="Stereo render and comparison retain both channel energies; offline transport follows explicit and default tempo.")

    def workspace(self, marker, outputs, user):
        path = "examples/workspace-target.flows"
        if marker == "TEST_WORKSPACE_NEW":
            assert "Optional instrument reference:" not in user
            if not outputs:
                source = "import standard as std\nInst.sigstream(std.event.keys.poly()) as key:\n    return key.gate * 0.1\n"
                self.completion(calls=[("new", "propose_source_patch", {"source": source, "explanation": "New instrument."})])
                return
            assert outputs["new"]["status"] == "proposal_validated"
        else:
            if marker == "TEST_WORKSPACE_REFERENCE":
                assert "Optional instrument reference: " + path in user and "unsaved-workspace-marker" in user
            else:
                assert "Optional instrument reference:" not in user
            if not outputs:
                self.completion(calls=[("search", "search_instruments", {"query": "unsaved-workspace-marker"}),
                                       ("escape", "read_source", {"path": "../outside.flows"}),
                                       ("hidden", "read_source", {"path": ".recovery/private.flows"})])
                return
            assert any(i["path"] == path and i["unsaved"] for i in outputs["search"]["instruments"]), "Search missed inactive unsaved draft"
            assert outputs["escape"]["status"] == "failed" and outputs["hidden"]["status"] == "failed"
            if "read" not in outputs:
                self.completion(calls=[("read", "read_source", {"path": path})])
                return
            read = outputs["read"]
            assert read["path"] == path and "unsaved-workspace-marker" in read["source"]
            if "wrong" not in outputs:
                self.completion(calls=[("wrong", "propose_source_patch", {"path": path, "base_digest": "wrong", "source": read["source"], "explanation": "Must reject wrong base."})])
                return
            assert outputs["wrong"]["status"] == "failed"
            if "edit" not in outputs:
                self.completion(calls=[("edit", "propose_source_patch", {"path": path, "base_digest": read["sourceDigest"], "source": read["source"].replace("-6db", "-12db"), "explanation": "Reduce target level."})])
                return
            assert outputs["edit"]["status"] == "proposal_validated"
        with self.server.lock:
            self.server.checked.add(marker)
        self.completion(text="Workspace tools completed. Review the named target.")

    def do_POST(self):
        try:
            assert self.path == "/v1/chat/completions", "Wrong endpoint path"
            assert self.headers.get("Authorization") == "Bearer secret-test-session", "Session authorization was not sent"
            length = int(self.headers.get("Content-Length", "0"))
            assert 0 < length <= 4 * 1024 * 1024, "Invalid request size"
            payload = json.loads(self.rfile.read(length))
            assert payload["model"] == "flows-fake"
            messages = payload["messages"]
            user = next(message["content"] for message in reversed(messages) if message["role"] == "user")
            assert "secret-test-session" not in json.dumps(payload), "Credential leaked into the model context"
            marker = user.splitlines()[0]
            if marker == "TEST_CANCEL":
                with self.server.lock:
                    self.server.checked.add("cancel")
                # Headers arrive immediately; the native worker must cancel a blocking read.
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", "1000")
                self.end_headers()
                self.wfile.flush()
                time.sleep(2)
                return
            if marker == "TEST_DEEP":
                with self.server.lock:
                    self.server.checked.add("deep")
                encoded = b"[" * 10000 + b"0" + b"]" * 10000
                self.send_response(200)
                self.send_header("Content-Length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)
                return
            if marker == "TEST_SECRET_ERROR":
                with self.server.lock:
                    self.server.checked.add("secret_error")
                self.reply({"error": {"message": "Invalid key secret-test-session"}}, 401)
                return
            if marker == "TEST_MALFORMED":
                with self.server.lock:
                    self.server.checked.add("malformed")
                self.send_response(200)
                self.send_header("Content-Length", "4")
                self.end_headers()
                self.wfile.write(b"nope")
                return
            if marker == "TEST_CONTEXT":
                assert messages[0]["role"] == "system" and "current code is authoritative" in messages[0]["content"]
                roles = [message["role"] for message in messages]
                assert roles == ["system", "user", "assistant", "user"], "History must replay only completed paired turns"
                assert messages[1]["content"].startswith("TEST_REPLY") and "Earlier instrument:" in messages[1]["content"]
                assert messages[2]["content"] == "Request completed after cancellation and provider failure."
                assert "tool_calls" not in messages[2] and "reasoning_content" not in messages[2]
                with self.server.lock:
                    self.server.checked.add("context")
                self.completion(text="The previous completed answer is present in this conversation.")
                return
            if marker == "TEST_REPLY":
                with self.server.lock:
                    self.server.checked.add("recovered")
                self.completion(text="Request completed after cancellation and provider failure.")
                return
            outputs = {message["tool_call_id"]: json.loads(message["content"]) for message in messages if message["role"] == "tool"}
            if marker.startswith("TEST_WORKSPACE"):
                self.workspace(marker, outputs, user)
                return
            if marker == "TEST_STEREO_RENDER":
                self.stereo_render(outputs)
                return
            assert marker in {"TEST_RENDER", "TEST_STALE"}, "Unexpected test prompt"
            source = user.split("```flows\n", 1)[1].rsplit("\n```", 1)[0]
            if not outputs:
                if marker == "TEST_STALE":
                    time.sleep(0.2)
                self.completion(calls=[("compile", "compile_snapshot", {}), ("render", "render_case", {"duration_seconds": 0.15, "pitch": 69})])
                return
            assert outputs["compile"]["status"] == "compiled", "Native compiler did not actually accept source"
            measured = outputs["render"]
            assert measured["status"] == "rendered" and measured["frames"] == 7200
            assert measured["sample_rate"] == 48000 and measured["acceptedNotes"] == 1
            assert measured["metrics"]["finite"] and measured["metrics"]["peak"] > 0.1 and measured["metrics"]["rms"] > 0.01, "Native tool did not produce measured audio"
            if "proposal" not in outputs:
                proposed = source.replace("level: -6db", "level: -12db")
                assert proposed != source, "Expected known source for level proposal"
                self.completion(calls=[("proposal", "propose_source_patch", {"source": proposed, "explanation": "Reduce sine amplitude by 6 dB."})])
                return
            assert outputs["proposal"]["status"] == "proposal_validated"
            assert outputs["proposal"]["applied"] is False
            with self.server.lock:
                self.server.checked.add("stale" if marker == "TEST_STALE" else "render_proposal")
            self.completion(text="The source compiled and the offline render produced finite audio. Review the level proposal before applying it.")
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as error:
            with self.server.lock:
                self.server.errors.append(str(error))
            try:
                self.reply({"error": {"message": "Fake endpoint assertion failed"}}, 500)
            except (BrokenPipeError, ConnectionResetError):
                pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    args = parser.parse_args()
    server = Peer()
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="flows-host-test-") as workspace:
            env = dict(os.environ, FLOWS_WORKSPACE=workspace)
            result = subprocess.run([str(args.binary.resolve()), "--endpoint", f"http://127.0.0.1:{server.server_port}/v1"], env=env, text=True, capture_output=True, timeout=75)
            if "secret-test-session" in result.stdout + result.stderr:
                raise AssertionError("Session API key leaked into native process output")
            print(result.stdout, end="")
            if result.returncode:
                print(result.stderr, end="")
                detail = "; ".join(server.errors).replace("secret-test-session", "[redacted]")
                raise AssertionError(f"Native host test exited {result.returncode}: {detail}")
            assert not server.errors, "; ".join(server.errors)
            expected = {"render_proposal", "stereo_render_tempo", "stale", "cancel", "malformed", "recovered", "deep", "secret_error", "context"}
            expected |= {"TEST_WORKSPACE", "TEST_WORKSPACE_REFERENCE", "TEST_WORKSPACE_NEW"}
            assert expected <= server.checked, f"Missing endpoint coverage: {expected - server.checked}"
            print("Local AI endpoint integration passed; no external account or API service was used.")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


if __name__ == "__main__":
    main()
