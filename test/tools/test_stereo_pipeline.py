#!/usr/bin/env python3
"""Stereo WAV / LFO / observation integration, independently decoded with stdlib."""
import argparse
import csv
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import wave

BINARY = None
ROOT = Path(__file__).resolve().parents[2]
RATE = 48000


class StereoPipeline(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='flows-stereo-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.events = self.root / 'events.tsv'
        self.events.write_text('0 note_on 1 57 0.8\n19200 note_off 1 57 0\n')
        self.source = (ROOT / 'test/presets/stereo-motion.flows').read_text()

    def render(self, name, source=None, block=64, bpm=120, projection='mid'):
        path = self.root / (name + '.flows')
        path.write_text(source or self.source)
        output = self.root / name
        done = subprocess.run([str(BINARY), 'render', str(path), '--events', str(self.events),
                               '--output', str(output), '--duration', '0.8', '--block-size', str(block),
                               '--bpm', str(bpm), '--capture-channel', projection,
                               '--observe', 'motion,shaped,width'], capture_output=True, text=True, timeout=30)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        with wave.open(str(output / 'output.wav')) as wav:
            self.assertEqual(wav.getnchannels(), 2)
            self.assertEqual(wav.getframerate(), RATE)
            self.assertEqual(wav.getsampwidth(), 2)
            values = struct.unpack('<' + str(wav.getnframes() * 2) + 'h', wav.readframes(wav.getnframes()))
        report = json.loads((output / 'engine.json').read_text())
        self.assertEqual(report['channels'], 2)
        self.assertEqual(report['clipped_samples'], 0)
        self.assertEqual(report['active_voices'], 0)
        ir = json.loads((output / 'program.ir.json').read_text())
        with (output / 'captures.csv').open() as f:
            captured = {(int(row['value_id']), int(row['sample'])): float(row['value']) for row in csv.DictReader(f)}
        return output, values, ir, captured

    def test_actual_stereo_and_partition_invariance(self):
        output, values, _, _ = self.render('reference')
        left, right = values[::2], values[1::2]
        self.assertGreater(sum((a-b)**2 for a,b in zip(left,right)) / len(left), 100)
        self.assertTrue(any(abs(x) > 100 for x in left) and any(abs(x) > 100 for x in right))
        for block in (1, 127):
            partition, _, _, _ = self.render('partition-' + str(block), block=block)
            self.assertEqual((output/'output.wav').read_bytes(), (partition/'output.wav').read_bytes())
        without_analysis = '\n'.join(line for line in self.source.splitlines()
                                     if not line.strip().startswith(('p_before =', 'p_after =', 'color_diff =', 'width_power ='))) + '\n'
        plain, _, _, _ = self.render('without-probes', source=without_analysis)
        self.assertEqual((output/'output.wav').read_bytes(), (plain/'output.wav').read_bytes())

    def test_projection_and_tempo(self):
        # Discover named ValueIds through captured IR rather than relying on node order.
        a, _, ir, left = self.render('left', projection='left')
        _, _, _, right = self.render('right', projection='right')
        _, _, _, side = self.render('side', projection='side')
        names = {item['name']: item['value'] for item in ir['named_values']}
        for sample in (6000, 9000, 12000):
            key = (names['shaped'], sample)
            self.assertAlmostEqual(side[key], (left[key]-right[key])/2, delta=1e-6)
            control = (names['motion'], sample)
            self.assertEqual(left[control], side[control], 'Audio projection must not change control signals')
        _, _, _, slow = self.render('slow', bpm=60)
        self.assertAlmostEqual(left[(names['motion'], 6000)], .5, delta=1e-4)
        self.assertAlmostEqual(slow[(names['motion'], 6000)], .25, delta=1e-4)
        self.assertEqual(json.loads((a/'engine.json').read_text())['capture_projection'], 'left')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', required=True, type=Path)
    args, remaining = parser.parse_known_args()
    BINARY = args.binary.resolve()
    unittest.main(argv=['stereo-pipeline', *remaining])
