#!/usr/bin/env python3
"""Compile and render the talk library; never infer manual listening quality."""
import argparse
import array
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import wave

REPO = Path(__file__).resolve().parents[2]


def command(args):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)


def wav_metrics(path):
    with wave.open(str(path), 'rb') as wav:
        channels, frames = wav.getnchannels(), wav.getnframes()
        assert wav.getsampwidth() == 2
        values = array.array('h', wav.readframes(frames))
    if sys.byteorder != 'little':
        values.byteswap()
    rms = math.sqrt(sum((sample / 32768) ** 2 for sample in values) / len(values))
    side_rms = math.sqrt(sum(((values[i] - values[i + 1]) / 65536) ** 2
                             for i in range(0, len(values), 2)) / frames) if channels == 2 else 0
    return {'rms': rms, 'side_rms': side_rms, 'channels': channels,
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=REPO / 'build-app/flows')
    parser.add_argument('--output', type=Path, default=REPO / 'test/output/adcxgather-validation')
    args = parser.parse_args()
    library = REPO / 'examples/ADCxGather'
    manifest = json.loads((library / 'manifest.json').read_text())
    listed = {entry['path'] for entry in manifest['examples']}
    actual = {str(path.relative_to(library)) for path in library.rglob('*.flows')}
    assert listed == actual, 'The catalogue and the instrument files must agree.'
    assert len(listed) == len(manifest['examples']), 'Catalogue paths must be unique.'
    assert set(manifest['demo_route']) <= listed, 'Each demo must name an existing instrument.'
    # Fail before compiling anything: a rerun must not mix fresh IR with old
    # audio and an earlier passed report.
    if args.output.exists() and (not args.output.is_dir() or any(args.output.iterdir())):
        parser.error('Output must be a new or empty directory; existing validation evidence is preserved.')
    args.output.mkdir(parents=True, exist_ok=True)
    report = {'status': 'passed', 'binary': str(args.binary.resolve()), 'manual_listening': 'not evaluated', 'examples': []}
    hashes = {}
    with tempfile.TemporaryDirectory(prefix='flows-gather-') as temporary:
        scratch = Path(temporary)
        for entry in manifest['examples']:
            source = library / entry['path']
            folder = args.output / Path(entry['path']).with_suffix('')
            folder.mkdir(parents=True, exist_ok=True)
            ir_path = folder / 'compile.ir.json'
            command([args.binary, 'compile', source, '--ir', ir_path])
            ir = json.loads(ir_path.read_text())
            # Every enabled dropdown must be a valid one-edit choice for this example.
            raw = source.read_bytes()
            choices = 0
            seen = set()
            for descriptor in ir['choices']:
                span = descriptor['source_span']
                for option in descriptor['options']:
                    if not option['enabled'] or option['value'] == descriptor['selected']:
                        continue
                    replacement = (descriptor['prefix'] + option['value'] + descriptor['suffix']).encode()
                    key = (span['byte_offset'], span['byte_length'], replacement)
                    if key in seen:
                        continue
                    seen.add(key)
                    begin, length = key[:2]
                    changed = scratch / 'choice.flows'
                    changed.write_bytes(raw[:begin] + replacement + raw[begin + length:])
                    command([args.binary, 'compile', changed, '--ir', scratch / 'choice.ir.json'])
                    choices += 1
            rendered = []
            for rate, block in ((48000, 64), (44100, 127)):
                events = scratch / 'notes.tsv'
                sequence = [(0, 'note_on', 1, 48, .65), (.45, 'note_on', 2, 52, .5),
                            (.8, 'note_on', 3, 55, .5), (1.2, 'note_off', 2, 52, 0),
                            (1.5, 'note_off', 3, 55, 0), (1.8, 'note_off', 1, 48, 0)]
                events.write_text(''.join(f'{round(time * rate)} {kind} {identity} {pitch} {velocity}\n'
                                          for time, kind, identity, pitch, velocity in sequence))
                output = folder / f'{rate}-{block}'
                command([args.binary, 'render', source, '--events', events, '--output', output,
                         '--duration', '3.2', '--sample-rate', rate, '--block-size', block, '--bpm', '138'])
                engine = json.loads((output / 'engine.json').read_text())
                metrics = wav_metrics(output / 'output.wav')
                assert engine['accepted_notes'] == 3 and engine['rejected_notes'] == 0, entry['path']
                assert engine['active_voices'] == 0, f'Unfinished voice in {entry["path"]}'
                assert engine['clipped_samples'] == 0, f'Clipping in {entry["path"]}'
                assert math.isfinite(engine['peak_before_quantization']) and metrics['rms'] > 1e-5, entry['path']
                if 'stereo' in entry['path']:
                    assert metrics['channels'] == 2 and metrics['side_rms'] > 1e-5, entry['path']
                rendered.append({'sample_rate': rate, 'block_size': block,
                                 'peak': engine['peak_before_quantization'], **metrics})
                hashes[(entry['path'], rate, block)] = metrics['sha256']
            report['examples'].append({'path': entry['path'], 'enabled_alternative_choices': choices, 'renders': rendered})
            print(f'PASS {entry["path"]}: {choices} alternative choices, 2 finite renders, no clipping, all voices released')
    for rate, block in ((48000, 64), (44100, 127)):
        assert hashes[('04-probe/01-filter-before-after.flows', rate, block)] == hashes[('04-probe/02-probe-difference.flows', rate, block)], 'Adding analysis expressions changed the audio.'
    report['probe_pair_audio_identical'] = True
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'{len(report["examples"])} instruments passed; the before/after and difference examples produce identical audio.')


if __name__ == '__main__':
    main()
