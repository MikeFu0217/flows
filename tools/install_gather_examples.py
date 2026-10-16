#!/usr/bin/env python3
"""Copy the ADCxGather library into Flows without replacing user files."""
import argparse
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def install(source: Path, target: Path) -> dict:
    manifest = json.loads((source / 'manifest.json').read_text())
    names = [entry['path'] for entry in manifest['examples']] + ['manifest.json', 'README.md']
    target.mkdir(parents=True, exist_ok=True)
    root = target.resolve()
    report = {'workspace_library': str(root), 'copied': [], 'unchanged': [], 'preserved': []}
    for name in names:
        relative = Path(name)
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError(f'Invalid library path: {name}')
        destination = target / relative
        if not destination.resolve().is_relative_to(root):
            raise ValueError(f'Destination escapes library through a symbolic link: {destination}')
        data = (source / relative).read_bytes()
        destination.parent.mkdir(parents=True, exist_ok=True)
        try:
            with destination.open('xb') as handle:
                handle.write(data)
            report['copied'].append(name)
        except FileExistsError:
            category = 'unchanged' if destination.is_file() and destination.read_bytes() == data else 'preserved'
            report[category].append(name)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=REPO / 'examples/ADCxGather')
    parser.add_argument('--target', type=Path, default=Path.home() / 'Library/Flows/Workspace/tutorial')
    args = parser.parse_args()
    report = install(args.source, args.target)
    print(json.dumps(report, indent=2))
    if report['preserved']:
        print('Existing edits were preserved. Choose a new --target for a clean copy.')


if __name__ == '__main__':
    main()
