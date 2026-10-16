#!/usr/bin/env python3
"""Package a tagged, tested macOS release with source and dependency licenses."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = (ROOT / 'VERSION').read_text().strip()
BUNDLES = (
    ('FlowsStandalone_artefacts/Release/Flows.app', 'Flows.app'),
    ('FlowsPlugin_artefacts/Release/AU/Flows.component', 'Plugins/AU/Flows.component'),
    ('FlowsPlugin_artefacts/Release/VST3/Flows.vst3', 'Plugins/VST3/Flows.vst3'),
)


def command(*args):
    return subprocess.check_output(args, text=True).strip()


def source_identity(tag):
    if command('git', '-C', str(ROOT), 'status', '--porcelain=v1', '--untracked-files=all'):
        raise ValueError('Commit all release source changes before packaging')
    revision = command('git', '-C', str(ROOT), 'rev-parse', 'HEAD^{commit}')
    tagged = command('git', '-C', str(ROOT), 'rev-parse', f'refs/tags/{tag}^{{commit}}')
    if revision != tagged:
        raise ValueError('The release tag must point at HEAD')
    for name in ('ui/package.json', 'ui/package-lock.json'):
        if json.loads((ROOT / name).read_text())['version'] != VERSION:
            raise ValueError(f'Version mismatch in {name}')
    return revision


def inventory(root):
    files = {}
    for p in sorted(root.rglob('*')):
        name = p.relative_to(root).as_posix()
        if p.is_symlink():
            if not p.resolve().is_relative_to(root.resolve()):
                raise ValueError(f'Package symlink escapes its root: {name}')
            files[name] = {'symlink': os.readlink(p)}
        elif p.is_file():
            files[name] = {'sha256': hashlib.sha256(p.read_bytes()).hexdigest(), 'bytes': p.stat().st_size}
    return files


def archive(directory, target):
    with zipfile.ZipFile(target, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in [directory, *sorted(directory.rglob('*'))]:
            link = p.is_symlink(); is_directory = p.is_dir() and not link
            name = p.relative_to(directory.parent).as_posix() + ('/' if is_directory else '')
            item = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            item.create_system = 3; item.external_attr = p.lstat().st_mode << 16
            if is_directory:
                item.external_attr |= 0x10
            item.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(item, os.readlink(p).encode() if link else b'' if is_directory else p.read_bytes())


def package(build, output, tag, source_url, juce):
    revision = source_identity(tag)
    if command('git', '-C', str(juce), 'rev-parse', 'HEAD') != '10a589619b452c261b2940767eb253171eb5a823':
        raise ValueError('Packaging requires the pinned JUCE 8.0.7 checkout')
    if command('git', '-C', str(juce), 'status', '--porcelain=v1', '--untracked-files=no'):
        raise ValueError('The pinned JUCE source must have no tracked modifications')
    subprocess.run(['python3', str(ROOT / 'tools/publication_check.py')], check=True)
    if not (ROOT / 'licenses/third-party/ui.txt').is_file():
        raise ValueError('Run tools/collect_licenses.py before committing the release')
    name = f'Flows-{VERSION}-macOS-arm64'; output.mkdir(parents=True, exist_ok=True)
    final = output / name; zipped = output / (name + '.zip')
    if final.exists() or zipped.exists():
        raise ValueError('Choose an empty output directory; existing packages are never overwritten')
    with tempfile.TemporaryDirectory(prefix='.flows-package-', dir=output) as temporary:
        staging = Path(temporary) / name; staging.mkdir()
        bundles = {}
        for source, relative in BUNDLES:
            bundle = staging / relative
            shutil.copytree(build / source, bundle, symlinks=True)
            plist = bundle / 'Contents/Info.plist'
            info = plistlib.loads(plist.read_bytes())
            if info['CFBundleShortVersionString'] != VERSION:
                raise ValueError(f'Build version does not match VERSION: {relative}')
            executable = bundle / 'Contents/MacOS' / info['CFBundleExecutable']
            if command('/usr/bin/lipo', '-archs', str(executable)) != 'arm64':
                raise ValueError('This package targets Apple Silicon only')
            info['FlowsSourceCommit'] = revision; info['FlowsReleaseTag'] = tag
            plist.write_bytes(plistlib.dumps(info))
            legal = bundle / 'Contents/Resources/Licenses'
            shutil.copytree(ROOT / 'licenses', legal)
            for text in ('LICENSE', 'NOTICE', 'THIRD_PARTY.md'):
                shutil.copyfile(ROOT / text, legal / text)
            (legal / 'SOURCE.txt').write_text(f'Corresponding source: {source_url}/tree/{tag}\nCommit: {revision}\n')
            command('/usr/bin/codesign', '--force', '--deep', '--sign', '-', '--timestamp=none', str(bundle))
            command('/usr/bin/codesign', '--verify', '--deep', '--strict', str(bundle))
            bundles[relative] = {'architectures': ['arm64'], 'codesign_strict_verified': True}
        source_archive = staging / f'Flows-{VERSION}-source.tar.gz'
        source_tar = subprocess.check_output(['git', '-C', str(ROOT), 'archive', '--format=tar', f'--prefix=Flows-{VERSION}/', tag])
        with source_archive.open('wb') as stream, gzip.GzipFile(fileobj=stream, mode='wb', filename='', mtime=0) as packed:
            packed.write(source_tar)
        juce_tar = subprocess.check_output(['git', '-C', str(juce), 'archive', '--format=tar', '--prefix=JUCE-8.0.7/', 'HEAD'])
        with (staging / 'JUCE-8.0.7-source.tar.gz').open('wb') as stream, gzip.GzipFile(fileobj=stream, mode='wb', filename='', mtime=0) as packed:
            packed.write(juce_tar)
        for text in ('LICENSE', 'NOTICE', 'THIRD_PARTY.md', 'BUILDING.md'):
            shutil.copyfile(ROOT / text, staging / text)
        (staging / 'README.txt').write_text(f'''Flows {VERSION} — experimental macOS preview

Apple Silicon only. Built with a macOS 11 deployment target; older OS runtime
compatibility has not been established. This local build is ad-hoc signed and
not notarized. Move Flows.app to Applications, open an example and press Cmd+K
for Perform mode. Computer keys or the piano/MIDI keyboard play the instrument.
AI is optional and needs your own endpoint/model/key. No credentials or personal
workspace are included. Valid unsaved source drives sound; compile errors mute it.

AU/VST3 bundles are under Plugins. They are not automatically installed.
Test your DAW/device separately; no universal host compatibility is claimed.

The complete application is AGPL-3.0-only. Independent compiler/DSP directories
are Apache-2.0. Upstream dependency licenses are included inside each bundle.
Flows source and build instructions are included in the source archive.
The exact JUCE source used for this build is in JUCE-8.0.7-source.tar.gz.
Extract both archives beside each other and set JUCE_DIR to the JUCE folder
when following BUILDING.md. The UI lockfile identifies its npm dependencies.
Source URL: {source_url}/tree/{tag}
Public commit: {revision}
''')
        manifest = {'version': VERSION, 'release_tag': tag, 'source_commit': revision,
                    'source_url': source_url, 'bundles': bundles, 'files': inventory(staging)}
        (staging / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        if source_identity(tag) != revision:
            raise ValueError('Release source changed during packaging')
        staging.rename(final)
    archive(final, zipped)
    print(json.dumps({'directory': str(final), 'zip': str(zipped), 'bytes': zipped.stat().st_size,
                      'sha256': hashlib.sha256(zipped.read_bytes()).hexdigest()}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build-app')
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'dist')
    parser.add_argument('--tag', default=f'v{VERSION}')
    parser.add_argument('--source-url', default='https://github.com/flows-audio/flows')
    parser.add_argument('--juce-dir', type=Path, required=True)
    args = parser.parse_args()
    package(args.build_dir.resolve(), args.output_dir.resolve(), args.tag,
            args.source_url.rstrip('/'), args.juce_dir.resolve())
