#!/usr/bin/env python3
"""Preserve notices from the installed runtime packages and pinned JUCE checkout."""
import argparse
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
JUCE_COMMIT = '10a589619b452c261b2940767eb253171eb5a823'


def runtime_licenses():
    tree = json.loads(subprocess.check_output(
        ['npm', 'ls', '--omit=dev', '--all', '--json'], cwd=ROOT / 'ui', text=True))
    visited, entries = set(), []

    def walk(dependencies, parent):
        for name, dependency in sorted(dependencies.items()):
            if dependency.get('dev'):
                continue
            candidate = parent / 'node_modules' / name
            search = parent
            while not (candidate / 'package.json').is_file() and search != ROOT.parent:
                search = search.parent
                candidate = search / 'node_modules' / name
            if not (candidate / 'package.json').is_file():
                raise ValueError(f'Runtime dependency not installed: {name}')
            package = json.loads((candidate / 'package.json').read_text())
            identity = (name, package['version'])
            if identity not in visited:
                visited.add(identity)
                files = sorted(p for p in candidate.iterdir() if p.is_file()
                               and re.match(r'^(license|licence|copying|notice)(?:[.-]|$)', p.name, re.I))
                if not files:
                    raise ValueError(f'No license text for runtime package {name}@{package["version"]}')
                entries.append('\n'.join([
                    '=' * 72, f'{name}@{package["version"]}',
                    f'License: {package.get("license", package.get("licenses", "See text"))}',
                    *[f'\n--- {p.name} ---\n{p.read_text()}' for p in files],
                ]))
                walk(dependency.get('dependencies', {}), candidate)

    walk(tree.get('dependencies', {}), ROOT / 'ui')
    return '\n\n'.join(entries) + '\n', len(visited)


def collect(juce):
    commit = subprocess.check_output(['git', '-C', str(juce), 'rev-parse', 'HEAD'], text=True).strip()
    if commit != JUCE_COMMIT:
        raise ValueError('Use the pinned JUCE 8.0.7 checkout')
    destination = ROOT / 'licenses/third-party'
    destination.mkdir(parents=True, exist_ok=True)
    text, count = runtime_licenses()
    (destination / 'ui.txt').write_text(text)
    selected = [juce / 'LICENSE.md']
    for folder in ('juce_audio_formats', 'juce_graphics', 'juce_core',
                   'juce_audio_devices', 'juce_audio_processors',
                   'juce_audio_plugin_client/AU', 'juce_javascript'):
        selected.extend(p for p in (juce / 'modules' / folder).rglob('*') if p.is_file()
                        and re.search(r'licen[cs]e|^copying|^notice', p.name, re.I)
                        and p.suffix.lower() != '.pdf')
    selected.append(juce / 'modules/juce_graphics/image_formats/jpglib/README')
    for source in sorted(set(selected)):
        target = destination / 'juce' / source.relative_to(juce)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(source.read_bytes())
    # These dependencies keep their license in the source header itself.
    for relative in ('modules/juce_javascript/choc/javascript/choc_javascript_QuickJS.h',):
        source = juce / relative
        if source.is_file():
            content = source.read_text()
            notices = re.findall(r'CHOC_REGISTER_OPEN_SOURCE_LICENCE\s*\([^,]+,\s*R"\((.*?)\)"\)', content, re.S)
            if not notices:
                raise ValueError(f'Missing embedded license: {relative}')
            header = '\n\n'.join(notices)
            target = destination / 'juce' / (relative + '.license.txt')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(header + '\n')
    print(json.dumps({'runtime_packages': count, 'juce_notice_files': len(set(selected))}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--juce-dir', type=Path, required=True)
    collect(parser.parse_args().juce_dir.resolve())
