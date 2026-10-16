#!/usr/bin/env python3
"""Regression checks for publication privacy and deterministic release archives."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / (name + '.py'))
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value)
    return value


privacy = module('publication_check')
packaging = module('package_macos')


class PublicationTests(unittest.TestCase):
    def source(self, root):
        (root / 'VERSION').write_text('1.1.4\n')
        (root / 'ui').mkdir()
        for name in ('package.json', 'package-lock.json'):
            (root / 'ui' / name).write_text(json.dumps({'version': '1.1.4'}))

    def report(self, root, forbidden=()):
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream):
            passed = privacy.check(root, forbidden)
        return passed, json.loads(stream.getvalue()), stream.getvalue()

    def test_ignored_local_data_and_preserved_third_party_attribution(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); self.source(root)
            private = 'sk-' + 'x' * 30
            (root / 'build').mkdir(); (root / 'build/private.txt').write_text(private)
            (root / '.local').mkdir(); (root / '.local/private.txt').write_text(private)
            legal = root / 'licenses'; legal.mkdir()
            (legal / 'upstream.txt').write_text('License dated ' + '2007' + '-11-19')
            self.assertTrue(self.report(root)[0])

    def test_reports_never_reveal_matched_secrets_or_private_commits(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); self.source(root)
            private = 'sk-' + 'x' * 30; commit = 'a' * 40
            (root / 'example.txt').write_text(private + '\n' + commit + '\n' + '2020' + '-01-01')
            passed, report, output = self.report(root, [commit])
            self.assertFalse(passed)
            self.assertEqual({r['rule'] for r in report['findings']},
                             {'credential', 'private_commit_reference', 'development_date'})
            self.assertNotIn(private, output); self.assertNotIn(commit, output)

    def test_version_alignment_and_png_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); self.source(root)
            (root / 'ui/package.json').write_text(json.dumps({'version': 'other'}))
            data = b'\x89PNG\r\n\x1a\n' + struct.pack('>I', 0) + b'eXIf' + b'\0' * 4
            (root / 'image.png').write_bytes(data)
            passed, report, _ = self.report(root)
            self.assertFalse(passed)
            self.assertEqual({r['rule'] for r in report['findings']}, {'version_mismatch', 'png_metadata'})

    def test_archive_normalizes_timestamps_and_preserves_executable_mode(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary); root = base / 'Flows'; root.mkdir()
            executable = root / 'program'; executable.write_text('executable'); executable.chmod(0o755)
            first = base / 'first.zip'; second = base / 'second.zip'
            packaging.archive(root, first); packaging.archive(root, second)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            with zipfile.ZipFile(first) as archive:
                entry = archive.getinfo('Flows/program')
                self.assertEqual(entry.date_time, (1980, 1, 1, 0, 0, 0))
                self.assertEqual((entry.external_attr >> 16) & 0o777, 0o755)


if __name__ == '__main__':
    unittest.main()
