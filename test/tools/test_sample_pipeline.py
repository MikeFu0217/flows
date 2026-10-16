#!/usr/bin/env python3
"""Check portable sample playback through the CLI, WAV output and probe captures."""
import argparse
import csv
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import wave

BINARY = None
ROOT = Path(__file__).resolve().parents[2]


class SamplePipeline(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='flows-sample-pipeline-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'Samples').mkdir()
        (self.root / 'Instruments').mkdir()
        for file in (ROOT / 'test/fixtures/samples').glob('*.wav'):
            shutil.copyfile(file, self.root / 'Samples' / file.name)

    def run_cli(self, *args):
        return subprocess.run([str(BINARY), *map(str, args)], capture_output=True, text=True, timeout=30)

    def render(self, name, preset='sample-one-shot', block=64, pitch=48, probe=True):
        source = (ROOT / 'test/presets' / (preset + '.flows')).read_text()
        if not probe:
            source = '\n'.join(line for line in source.splitlines() if not line.strip().startswith(('p_raw =', 'p_before =', 'p_after =', 'color_diff ='))) + '\n'
        path = self.root / 'Instruments' / (name + '.flows')
        path.write_text(source)
        events = self.root / (name + '.tsv')
        events.write_text(f'0 note_on 1 {pitch} 0.8\n12000 note_off 1 {pitch} 0\n')
        output = self.root / name
        result = self.run_cli('render', path, '--asset-root', self.root, '--events', events,
                              '--output', output, '--duration', '.8', '--block-size', block, '--observe', 'raw')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with wave.open(str(output / 'output.wav')) as wav:
            self.assertEqual(wav.getnchannels(), 2)
            values = struct.unpack('<' + str(wav.getnframes() * 2) + 'h', wav.readframes(wav.getnframes()))
        report = json.loads((output / 'engine.json').read_text())
        self.assertEqual(report['active_voices'], 0)
        self.assertEqual(report['completed_notes'], 1)
        self.assertEqual(report['clipped_samples'], 0)
        self.assertGreater(max(map(abs, values)), 100)
        return output, values

    def test_one_shot_partition_probe_and_transposition(self):
        output, values = self.render('one-shot')
        self.assertEqual(values[::2], values[1::2], 'Mono resources must broadcast to stereo')
        self.assertGreater(max(map(abs, values[26000:30000])), 10, 'Note-off must not cut a one-shot')
        self.assertTrue(all(value == 0 for value in values[58000:]))
        for block in (1, 127, 1024):
            other, _ = self.render(f'block-{block}', block=block)
            self.assertEqual((output / 'output.wav').read_bytes(), (other / 'output.wav').read_bytes())
        plain, _ = self.render('plain', probe=False)
        self.assertEqual((output / 'output.wav').read_bytes(), (plain / 'output.wav').read_bytes())
        _, high = self.render('octave', pitch=60)
        self.assertTrue(all(value == 0 for value in high[30000:]), 'An octave up must finish in half the time')
        ir = json.loads((output / 'program.ir.json').read_text())
        self.assertEqual(ir['samples'][0]['path'], 'Samples/flows-kick.wav')
        raw = next(value['value'] for value in ir['named_values'] if value['name'] == 'raw')
        with (output / 'captures.csv').open() as file:
            capture = [float(row['value']) for row in csv.DictReader(file) if int(row['value_id']) == raw]
        self.assertGreater(max(map(abs, capture)), .1, 'Sample probes must contain actual audio')

    def test_loop_processing_and_release(self):
        output, values = self.render('loop', preset='sample-loop')
        self.assertNotEqual(values[::2], values[1::2], 'Stereo samples must retain their channels through effects')
        self.assertGreater(max(map(abs, values[26000:30000])), 10, 'ADSR release must continue the loop')
        other, _ = self.render('loop-partition', preset='sample-loop', block=127)
        self.assertEqual((output / 'output.wav').read_bytes(), (other / 'output.wav').read_bytes())

    def test_compile_resolves_assets_and_reports_missing(self):
        path = self.root / 'Instruments' / 'missing.flows'
        path.write_text((ROOT / 'test/presets/sample-one-shot.flows').read_text())
        implicit = self.run_cli('compile', path)
        self.assertNotEqual(implicit.returncode, 0, 'A nested source needs its explicit workspace asset root')
        explicit = self.run_cli('compile', path, '--asset-root', self.root)
        self.assertEqual(explicit.returncode, 0, explicit.stderr)
        (self.root / 'Samples/flows-kick.wav').unlink()
        missing = self.run_cli('compile', path, '--asset-root', self.root)
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn('Samples/flows-kick.wav', missing.stderr)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', required=True, type=Path)
    args, remaining = parser.parse_known_args()
    BINARY = args.binary.resolve()
    unittest.main(argv=['sample-pipeline', *remaining])
